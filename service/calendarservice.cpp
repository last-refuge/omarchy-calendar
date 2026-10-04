#include "calendarservice.h"

#include "database.h"
#include "googleauth.h"
#include "googlesync.h"
#include "googlemutations.h"

#include <QFile>
#include <QDBusInterface>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QTimer>

CalendarService::CalendarService(Database &database, GoogleAuth &googleAuth, GoogleSync &googleSync,
                                 GoogleMutations &googleMutations,
                                 QString feedPath, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_googleAuth(googleAuth)
    , m_googleSync(googleSync)
    , m_googleMutations(googleMutations)
    , m_feedPath(std::move(feedPath))
{
    if (!m_database.finalizeUndoableDeletes())
        qWarning().noquote() << m_database.lastError();
    if (!m_database.pruneReminderDeliveries(
            QDateTime::currentMSecsSinceEpoch() - qint64(7) * 24 * 60 * 60 * 1000))
        qWarning().noquote() << m_database.lastError();
    m_mutationUploadDelay.setSingleShot(true);
    m_mutationUploadDelay.setInterval(450);
    connect(&m_mutationUploadDelay, &QTimer::timeout, this, [this] {
        if (m_googleAuth.writeAccessAvailable())
            m_googleMutations.start(m_googleAuth.currentAccountId(), m_googleAuth.accessToken());
    });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &CalendarService::feedChanged);
    connect(&m_googleAuth, &GoogleAuth::authorizationRequired,
            this, &CalendarService::AuthorizationRequired);
    connect(&m_googleAuth, &GoogleAuth::accountConnected, this, [this] {
        emit AccountsChanged();
        emit ProviderStatusChanged();
    });
    connect(&m_googleAuth, &GoogleAuth::accountDisconnected, this, [this] {
        emit AccountsChanged();
        emit EventsChanged();
        emit ProviderStatusChanged();
    });
    connect(&m_googleAuth, &GoogleAuth::stateChanged,
            this, &CalendarService::ProviderStatusChanged);
    connect(&m_googleAuth, &GoogleAuth::accessTokenReady,
            this, [this](const QString &accountId, const QString &accessToken) {
        m_googleSync.start(accountId, accessToken);
        if (m_googleAuth.writeAccessAvailable())
            m_googleMutations.start(accountId, accessToken);
        emit ProviderStatusChanged();
    });
    connect(&m_googleSync, &GoogleSync::stateChanged,
            this, &CalendarService::ProviderStatusChanged);
    connect(&m_googleSync, &GoogleSync::finished, this, [this](bool changed) {
        emit AccountsChanged();
        emit ProviderStatusChanged();
        if (changed) {
            if (!m_database.exportCompatibilityFeed(m_feedPath))
                qWarning().noquote() << m_database.lastError();
            ensureWatching();
            emit EventsChanged();
            notifyNewInvitations();
        }
    });
    connect(&m_googleMutations, &GoogleMutations::stateChanged,
            this, &CalendarService::ProviderStatusChanged);
    connect(&m_googleMutations, &GoogleMutations::finished, this, [this](bool changed) {
        emit ProviderStatusChanged();
        if (changed) {
            if (!m_database.exportCompatibilityFeed(m_feedPath))
                qWarning().noquote() << m_database.lastError();
            ensureWatching();
            emit EventsChanged();
            QTimer::singleShot(1000, this, [this] { SyncNow(); });
        }
    });
    auto *syncTimer = new QTimer(this);
    syncTimer->setInterval(5 * 60 * 1000);
    connect(syncTimer, &QTimer::timeout, this, [this] { SyncNow(); });
    syncTimer->start();
    m_reminderTimer.setInterval(30000);
    connect(&m_reminderTimer, &QTimer::timeout, this, &CalendarService::checkReminders);
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("ActionInvoked"),
        this, SLOT(notificationActionInvoked(uint,QString)));
    m_reminderTimer.start();
    QTimer::singleShot(1000, this, &CalendarService::checkReminders);
    ensureWatching();
}

void CalendarService::checkReminders()
{
    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"));
    if (!notifications.isValid()) return;
    const QJsonArray reminders = m_database.dueReminders(QDateTime::currentMSecsSinceEpoch()).array();
    for (const QJsonValue &value : reminders) {
        const QJsonObject reminder = value.toObject();
        const QDateTime start = QDateTime::fromMSecsSinceEpoch(
            qint64(reminder.value(QStringLiteral("startMs")).toDouble()));
        const QString when = reminder.value(QStringLiteral("allDay")).toBool()
            ? start.date().toString(QStringLiteral("ddd, MMM d")) + QStringLiteral(" · all day")
            : start.toString(QStringLiteral("h:mm AP"));
        const bool hasJoin = !reminder.value(QStringLiteral("meetingLinks")).toArray().isEmpty();
        QStringList actions { QStringLiteral("default"), QStringLiteral("Open") };
        if (hasJoin) actions << QStringLiteral("join") << QStringLiteral("Join");
        actions << QStringLiteral("snooze") << QStringLiteral("Snooze 10 min")
                << QStringLiteral("dismiss") << QStringLiteral("Dismiss");
        QVariantMap hints;
        hints.insert(QStringLiteral("category"), QStringLiteral("x-omarchy-calendar.reminder"));
        hints.insert(QStringLiteral("desktop-entry"), QStringLiteral("org.omarchy.Calendar"));
        const QDBusMessage response = notifications.call(QStringLiteral("Notify"),
            QStringLiteral("Omarchy Calendar"), uint(0), QStringLiteral("org.omarchy.Calendar"),
            reminder.value(QStringLiteral("title")).toString(QStringLiteral("Calendar reminder")),
            when + QStringLiteral(" · ") + reminder.value(QStringLiteral("calendarName")).toString(),
            actions, hints, 0);
        if (response.type() == QDBusMessage::ReplyMessage && !response.arguments().isEmpty()) {
            const uint notificationId = response.arguments().first().toUInt();
            if (m_database.markReminderDelivered(
                    reminder.value(QStringLiteral("id")).toString(), notificationId))
                m_activeReminders.insert(notificationId, reminder);
        }
    }
}

void CalendarService::notificationActionInvoked(uint notificationId, const QString &actionKey)
{
    const QString reminderId = m_database.reminderIdForNotification(notificationId);
    if (reminderId.isEmpty()) return;
    const QJsonObject reminder = m_activeReminders.value(notificationId);
    if (actionKey == QStringLiteral("snooze")) {
        m_database.snoozeReminder(reminderId, QDateTime::currentMSecsSinceEpoch() + 10 * 60 * 1000);
    } else {
        m_database.dismissReminder(reminderId);
        if (actionKey == QStringLiteral("join")) {
            const QJsonArray links = reminder.value(QStringLiteral("meetingLinks")).toArray();
            if (!links.isEmpty()) {
                QProcess::startDetached(QStringLiteral("xdg-open"),
                    { links.first().toObject().value(QStringLiteral("uri")).toString() });
            } else {
                openCalendarApp();
            }
        } else if (actionKey == QStringLiteral("open") || actionKey == QStringLiteral("default")) {
            openCalendarApp();
        }
    }
    m_activeReminders.remove(notificationId);
}

void CalendarService::openCalendarApp()
{
    QString executable = QStandardPaths::findExecutable(QStringLiteral("omarchy-calendar"));
    if (executable.isEmpty()) {
        executable = QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("../build-qmake/omarchy-calendar"));
    }
    QProcess::startDetached(executable, {});
}

void CalendarService::notifyNewInvitations()
{
    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"));
    if (!notifications.isValid()) return;
    const QJsonArray invitations = m_database.takeNewInvitations().array();
    if (invitations.isEmpty()) return;
    for (const QJsonValue &value : invitations) {
        const QJsonObject invitation = value.toObject();
        const QDateTime start = QDateTime::fromMSecsSinceEpoch(
            qint64(invitation.value(QStringLiteral("startMs")).toDouble()));
        const QString when = invitation.value(QStringLiteral("allDay")).toBool()
            ? start.date().toString(QStringLiteral("ddd, MMM d")) + QStringLiteral(" · all day")
            : start.toString(QStringLiteral("ddd, MMM d · h:mm AP"));
        notifications.call(QDBus::NoBlock, QStringLiteral("Notify"),
                           QStringLiteral("Omarchy Calendar"), uint(0),
                           QStringLiteral("org.omarchy.Calendar"),
                           QStringLiteral("Calendar invitation"),
                           invitation.value(QStringLiteral("title")).toString()
                               + QStringLiteral("\n") + when,
                           QStringList {}, QVariantMap {}, 10000);
    }
}

void CalendarService::ensureWatching()
{
    if (QFile::exists(m_feedPath) && !m_watcher.files().contains(m_feedPath))
        m_watcher.addPath(m_feedPath);
}

void CalendarService::feedChanged()
{
    Reload();
    ensureWatching();
}

QString CalendarService::GetEvents(const QString &firstDate, const QString &lastDate) const
{
    return QString::fromUtf8(m_database.eventsForRange(firstDate, lastDate).toJson(QJsonDocument::Compact));
}

QString CalendarService::GetCalendars() const
{
    return QString::fromUtf8(m_database.calendars().toJson(QJsonDocument::Compact));
}

QString CalendarService::GetAccounts() const
{
    return QString::fromUtf8(m_database.accounts().toJson(QJsonDocument::Compact));
}

QString CalendarService::SearchEvents(const QString &queryText, int limit) const
{
    return QString::fromUtf8(m_database.searchEvents(queryText, limit).toJson(QJsonDocument::Compact));
}

QString CalendarService::GetNextEvent() const
{
    return QString::fromUtf8(m_database.nextEvent().toJson(QJsonDocument::Compact));
}

QString CalendarService::GetStatus() const
{
    return QString::fromUtf8(m_database.status().toJson(QJsonDocument::Compact));
}

QString CalendarService::GetPendingMutations() const
{
    return QString::fromUtf8(m_database.pendingMutations().toJson(QJsonDocument::Compact));
}

QString CalendarService::GetProviderStatus() const
{
    QJsonObject status = m_googleAuth.status().object();
    status.insert(QStringLiteral("sync"), m_googleSync.status().object());
    status.insert(QStringLiteral("mutations"), m_googleMutations.status().object());
    return QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Compact));
}

QString CalendarService::GetDiagnostics() const
{
    QJsonObject database = m_database.status().object();
    database.remove(QStringLiteral("databasePath"));
    QJsonObject result {
        { QStringLiteral("application"), QStringLiteral("Omarchy Calendar") },
        { QStringLiteral("version"), QCoreApplication::applicationVersion() },
        { QStringLiteral("generatedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
        { QStringLiteral("database"), database },
        { QStringLiteral("provider"), QJsonDocument::fromJson(GetProviderStatus().toUtf8()).object() },
        { QStringLiteral("serviceBacked"), true }
    };
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
}

bool CalendarService::BeginGoogleAuthorization()
{
    return m_googleAuth.beginAuthorization();
}

bool CalendarService::DisconnectGoogle()
{
    m_googleSync.cancel();
    m_googleMutations.cancel();
    if (!m_googleAuth.disconnectAccount())
        return false;
    if (!m_database.exportCompatibilityFeed(m_feedPath)) {
        qWarning().noquote() << m_database.lastError();
        return false;
    }
    ensureWatching();
    emit EventsChanged();
    return true;
}

bool CalendarService::SetCalendarSelected(const QString &calendarId, bool selected)
{
    if (!m_database.setCalendarSelected(calendarId, selected))
        return false;
    if (!m_database.exportCompatibilityFeed(m_feedPath))
        qWarning().noquote() << m_database.lastError();
    ensureWatching();
    emit EventsChanged();
    if (selected)
        SyncNow();
    return true;
}

QString CalendarService::CreateEvent(const QString &eventJson)
{
    const QJsonDocument document = QJsonDocument::fromJson(eventJson.toUtf8());
    if (!document.isObject())
        return {};
    const QString eventId = m_database.createPendingEvent(document.object());
    if (eventId.isEmpty())
        return {};
    if (!m_database.exportCompatibilityFeed(m_feedPath))
        qWarning().noquote() << m_database.lastError();
    ensureWatching();
    emit EventsChanged();
    if (m_googleAuth.writeAccessAvailable())
        m_googleMutations.start(m_googleAuth.currentAccountId(), m_googleAuth.accessToken());
    return eventId;
}

bool CalendarService::UpdateEvent(const QString &eventJson)
{
    const QJsonDocument document = QJsonDocument::fromJson(eventJson.toUtf8());
    if (!document.isObject() || !m_database.updatePendingEvent(document.object()))
        return false;
    if (!m_database.exportCompatibilityFeed(m_feedPath))
        qWarning().noquote() << m_database.lastError();
    ensureWatching();
    emit EventsChanged();
    if (m_googleAuth.writeAccessAvailable())
        m_mutationUploadDelay.start();
    return true;
}

bool CalendarService::RespondToInvitation(const QString &calendarId, const QString &eventId,
                                          const QString &responseStatus)
{
    if (!m_database.respondPendingEvent(calendarId, eventId, responseStatus)) return false;
    emit EventsChanged();
    emit ProviderStatusChanged();
    if (m_googleAuth.writeAccessAvailable())
        m_googleMutations.start(m_googleAuth.currentAccountId(), m_googleAuth.accessToken());
    return true;
}

QString CalendarService::DeleteEvent(const QString &calendarId, const QString &eventId)
{
    return DeleteEventScoped(QString::fromUtf8(QJsonDocument(QJsonObject {
        { QStringLiteral("calendarId"), calendarId },
        { QStringLiteral("id"), eventId },
        { QStringLiteral("scope"), QStringLiteral("occurrence") }
    }).toJson(QJsonDocument::Compact)));
}

QString CalendarService::DeleteEventScoped(const QString &eventJson)
{
    const QJsonDocument document = QJsonDocument::fromJson(eventJson.toUtf8());
    if (!document.isObject()) return {};
    const QString mutationId = m_database.deletePendingEvent(document.object());
    if (mutationId.isEmpty()) return {};
    if (!m_database.exportCompatibilityFeed(m_feedPath))
        qWarning().noquote() << m_database.lastError();
    ensureWatching();
    emit EventsChanged();
    emit ProviderStatusChanged();
    QTimer::singleShot(6000, this, [this, mutationId] {
        if (!m_database.finalizePendingDelete(mutationId)) return;
        emit ProviderStatusChanged();
        if (m_googleAuth.writeAccessAvailable())
            m_googleMutations.start(m_googleAuth.currentAccountId(), m_googleAuth.accessToken());
    });
    return mutationId;
}

bool CalendarService::UndoDelete(const QString &mutationId)
{
    if (!m_database.undoPendingDelete(mutationId)) return false;
    if (!m_database.exportCompatibilityFeed(m_feedPath))
        qWarning().noquote() << m_database.lastError();
    ensureWatching();
    emit EventsChanged();
    emit ProviderStatusChanged();
    return true;
}

bool CalendarService::RetryMutation(const QString &mutationId)
{
    if (!m_database.retryMutation(mutationId)) return false;
    m_googleMutations.cancel();
    emit ProviderStatusChanged();
    return !m_googleAuth.writeAccessAvailable()
        || m_googleMutations.start(m_googleAuth.currentAccountId(), m_googleAuth.accessToken());
}

bool CalendarService::DiscardMutation(const QString &mutationId)
{
    m_googleMutations.cancel();
    if (!m_database.discardMutation(mutationId)) return false;
    if (!m_database.exportCompatibilityFeed(m_feedPath))
        qWarning().noquote() << m_database.lastError();
    ensureWatching();
    emit EventsChanged();
    emit ProviderStatusChanged();
    SyncNow();
    return true;
}

bool CalendarService::SyncNow()
{
    return m_googleSync.start(m_googleAuth.currentAccountId(), m_googleAuth.accessToken());
}

bool CalendarService::Reload()
{
    if (!m_database.importCompatibilityFeed(m_feedPath))
        return false;
    emit EventsChanged();
    return true;
}
