#include "database.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <cstdio>

namespace {
QJsonObject fixture(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool containsTitle(const QJsonArray &events, const QString &title)
{
    for (const auto &value : events) {
        if (value.toObject().value(QStringLiteral("title")).toString() == title)
            return true;
    }
    return false;
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2)
        return 2;
    const QString fixtures = app.arguments().at(1);
    QTemporaryDir temporary;
    Database database(temporary.filePath(QStringLiteral("calendar.db")));
    if (!database.open())
        return 3;
    if (!database.importCompatibilityFeed(fixtures + QStringLiteral("/calendar-events-v1.json")))
        return 16;
    const QString accountId = QStringLiteral("google:test-account");
    if (!database.upsertAccount(accountId, QStringLiteral("google"), QStringLiteral("test-account"),
                                QStringLiteral("Test User"), QStringLiteral("test@example.com"),
                                QStringLiteral("connected")))
        return 4;

    const QJsonObject calendars = fixture(fixtures + QStringLiteral("/google-calendar-list.json"));
    if (!database.replaceGoogleCalendars(accountId, calendars.value(QStringLiteral("items")).toArray()))
        return 5;
    const auto storedCalendars = database.calendarsForAccount(accountId).array();
    if (storedCalendars.size() != 2)
        return 6;
    QString primaryId;
    for (const auto &value : storedCalendars) {
        const auto calendar = value.toObject();
        if (calendar.value(QStringLiteral("providerCalendarId")).toString() == QStringLiteral("primary@example.com"))
            primaryId = calendar.value(QStringLiteral("id")).toString();
    }
    if (primaryId.isEmpty())
        return 7;

    const QJsonObject full = fixture(fixtures + QStringLiteral("/google-events-full.json"));
    if (!database.applyGoogleEvents(accountId, primaryId,
                                    full.value(QStringLiteral("items")).toArray(),
                                    full.value(QStringLiteral("nextSyncToken")).toString(), true)) {
        std::fprintf(stderr, "%s\n", database.lastError().toUtf8().constData());
        return 8;
    }
    if (!database.updateAccountSyncState(accountId, QStringLiteral("idle"))) {
        std::fprintf(stderr, "%s\n", database.lastError().toUtf8().constData());
        return 17;
    }
    const auto firstRange = database.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21")).array();
    if (firstRange.size() != 3 || !containsTitle(firstRange, QStringLiteral("Design review"))
        || !containsTitle(firstRange, QStringLiteral("Conference")))
        return 9;
    const QJsonArray descriptionSearch = database.searchEvents(QStringLiteral("new calendar"), 20).array();
    const QJsonArray attendeeSearch = database.searchEvents(QStringLiteral("ada@example.com"), 20).array();
    const QJsonArray organizerSearch = database.searchEvents(QStringLiteral("organizer:owner@example.com"), 20).array();
    const QJsonArray responseSearch = database.searchEvents(QStringLiteral("response:accepted"), 20).array();
    const QJsonArray filteredSearch = database.searchEvents(
        QStringLiteral("calendar:Primary after:2026-09-20 before:2026-09-21"), 20).array();
    if (!containsTitle(descriptionSearch,
                       QStringLiteral("Design review"))
        || !containsTitle(attendeeSearch,
                          QStringLiteral("Design review"))
        || !containsTitle(organizerSearch,
                          QStringLiteral("Design review"))
        || !containsTitle(responseSearch,
                          QStringLiteral("Design review"))
        || !containsTitle(filteredSearch,
                          QStringLiteral("Design review")))
    {
        std::fprintf(stderr, "search description=%lld attendee=%lld organizer=%lld response=%lld filtered=%lld error=%s\n",
                     qlonglong(descriptionSearch.size()), qlonglong(attendeeSearch.size()),
                     qlonglong(organizerSearch.size()), qlonglong(responseSearch.size()),
                     qlonglong(filteredSearch.size()), qPrintable(database.lastError()));
        return 29;
    }
    if (database.syncCursor(accountId, primaryId) != QStringLiteral("sync-token-1"))
        return 10;
    if (database.calendars().array().size() != 2)
        return 18;
    const QString exportedFeed = temporary.filePath(QStringLiteral("exported-feed.json"));
    if (!database.exportCompatibilityFeed(exportedFeed))
        return 19;
    const QJsonObject exported = fixture(exportedFeed);
    if (exported.value(QStringLiteral("version")).toInt() != 1
        || exported.value(QStringLiteral("source")).toString() != QStringLiteral("Omarchy Calendar")
        || exported.value(QStringLiteral("events")).toArray().size() != 3)
        return 20;
    if (!database.importCompatibilityFeed(exportedFeed)
        || database.calendars().array().size() != 2
        || database.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21")).array().size() != 3)
        return 25;
    if (!database.setCalendarSelected(primaryId, false)
        || !database.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21")).array().isEmpty())
        return 21;
    if (!database.exportCompatibilityFeed(exportedFeed)
        || !fixture(exportedFeed).value(QStringLiteral("events")).toArray().isEmpty())
        return 22;
    if (!database.replaceGoogleCalendars(accountId, calendars.value(QStringLiteral("items")).toArray()))
        return 23;
    bool primaryStillPaused = false;
    for (const auto &value : database.calendarsForAccount(accountId).array()) {
        const auto calendar = value.toObject();
        if (calendar.value(QStringLiteral("id")).toString() == primaryId)
            primaryStillPaused = !calendar.value(QStringLiteral("selected")).toBool();
    }
    if (!primaryStillPaused || !database.setCalendarSelected(primaryId, true)
        || database.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21")).array().size() != 3)
        return 24;

    const QJsonObject incremental = fixture(fixtures + QStringLiteral("/google-events-incremental.json"));
    if (!database.applyGoogleEvents(accountId, primaryId,
                                    incremental.value(QStringLiteral("items")).toArray(),
                                    incremental.value(QStringLiteral("nextSyncToken")).toString(), false)) {
        std::fprintf(stderr, "%s\n", database.lastError().toUtf8().constData());
        return 11;
    }
    const auto secondRange = database.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21")).array();
    if (secondRange.size() != 3 || containsTitle(secondRange, QStringLiteral("Design review"))
        || !containsTitle(secondRange, QStringLiteral("Conference updated"))
        || !containsTitle(secondRange, QStringLiteral("Planning session")))
        return 12;
    if (database.syncCursor(accountId, primaryId) != QStringLiteral("sync-token-2"))
        return 13;
    const QString localEventId = database.createPendingEvent(QJsonObject {
        { QStringLiteral("calendarId"), primaryId },
        { QStringLiteral("title"), QStringLiteral("Queued planning event") },
        { QStringLiteral("location"), QStringLiteral("Local office") },
        { QStringLiteral("startMs"), QDateTime::fromString(QStringLiteral("2026-09-21T15:00:00Z"), Qt::ISODate).toMSecsSinceEpoch() },
        { QStringLiteral("endMs"), QDateTime::fromString(QStringLiteral("2026-09-21T16:00:00Z"), Qt::ISODate).toMSecsSinceEpoch() }
    });
    const QJsonObject status = database.status().object();
    if (!localEventId.startsWith(QStringLiteral("local:"))
        || status.value(QStringLiteral("schemaVersion")).toInt() != 10
        || status.value(QStringLiteral("pendingMutationCount")).toInt() != 1
        || !containsTitle(database.eventsForRange(QStringLiteral("2026-09-21"), QStringLiteral("2026-09-21")).array(),
                          QStringLiteral("Queued planning event"))) {
        std::fprintf(stderr, "create=%s schema=%d pending=%d error=%s\n",
                     localEventId.toUtf8().constData(),
                     status.value(QStringLiteral("schemaVersion")).toInt(),
                     status.value(QStringLiteral("pendingMutationCount")).toInt(),
                     database.lastError().toUtf8().constData());
        return 26;
    }
    const QJsonObject queued = database.nextPendingMutation(accountId).object();
    if (queued.value(QStringLiteral("providerEventId")).toString() != localEventId
        || queued.value(QStringLiteral("operation")).toString() != QStringLiteral("create")
        || queued.value(QStringLiteral("payload")).toObject().value(QStringLiteral("title")).toString()
            != QStringLiteral("Queued planning event"))
        return 27;
    const QString queuedId = queued.value(QStringLiteral("id")).toString();
    const QJsonArray visibleQueue = database.pendingMutations().array();
    if (visibleQueue.size() != 1
        || visibleQueue.at(0).toObject().value(QStringLiteral("title"))
               != QStringLiteral("Queued planning event")
        || !database.setMutationState(queuedId, QStringLiteral("failed"),
                                      QStringLiteral("fixture failure"), true)
        || !database.pendingMutations().array().at(0).toObject().value(QStringLiteral("canRetry")).toBool()
        || !database.retryMutation(queuedId)
        || database.nextPendingMutation(accountId).object().value(QStringLiteral("state"))
               != QStringLiteral("queued"))
        return 30;
    if (!database.completeCreateMutation(queued.value(QStringLiteral("id")).toString(), QJsonObject {
            { QStringLiteral("id"), QStringLiteral("google-created-event") },
            { QStringLiteral("iCalUID"), QStringLiteral("created@example.com") },
            { QStringLiteral("htmlLink"), QStringLiteral("https://calendar.google.com/event?eid=created") },
            { QStringLiteral("etag"), QStringLiteral("created-etag") },
            { QStringLiteral("updated"), QStringLiteral("2026-09-20T23:00:00Z") }
        })
        || !database.nextPendingMutation(accountId).object().isEmpty()
        || database.status().object().value(QStringLiteral("pendingMutationCount")).toInt() != 0)
        return 28;

    QString deleteMutation = database.deletePendingEvent(primaryId, QStringLiteral("google-created-event"));
    if (deleteMutation.isEmpty() || !database.finalizePendingDelete(deleteMutation)
        || database.pendingMutations().array().size() != 1
        || !database.discardMutation(deleteMutation)
        || !containsTitle(database.eventsForRange(QStringLiteral("2026-09-21"),
                                                  QStringLiteral("2026-09-21")).array(),
                          QStringLiteral("Queued planning event")))
        return 31;

    const QString discardedCreate = database.createPendingEvent(QJsonObject {
        { QStringLiteral("calendarId"), primaryId },
        { QStringLiteral("title"), QStringLiteral("Never upload") },
        { QStringLiteral("startMs"), QDateTime::fromString(QStringLiteral("2026-09-21T17:00:00Z"), Qt::ISODate).toMSecsSinceEpoch() },
        { QStringLiteral("endMs"), QDateTime::fromString(QStringLiteral("2026-09-21T18:00:00Z"), Qt::ISODate).toMSecsSinceEpoch() }
    });
    const QJsonObject discardedMutation = database.nextPendingMutation(accountId).object();
    if (discardedCreate.isEmpty() || discardedMutation.isEmpty()
        || !database.discardMutation(discardedMutation.value(QStringLiteral("id")).toString())
        || !database.pendingMutations().array().isEmpty()
        || containsTitle(database.eventsForRange(QStringLiteral("2026-09-21"),
                                                 QStringLiteral("2026-09-21")).array(),
                         QStringLiteral("Never upload")))
        return 32;

    const auto queueFixture = [&](const QString &title, int hour) {
        return database.createPendingEvent(QJsonObject {
            { QStringLiteral("calendarId"), primaryId }, { QStringLiteral("title"), title },
            { QStringLiteral("startMs"), QDateTime(QDate(2026, 9, 22), QTime(hour, 0), QTimeZone::UTC).toMSecsSinceEpoch() },
            { QStringLiteral("endMs"), QDateTime(QDate(2026, 9, 22), QTime(hour + 1, 0), QTimeZone::UTC).toMSecsSinceEpoch() }
        });
    };
    if (queueFixture(QStringLiteral("First queued"), 9).isEmpty()) return 33;
    const QString firstQueuedId = database.nextPendingMutation(accountId).object().value("id").toString();
    if (queueFixture(QStringLiteral("Second queued"), 11).isEmpty()
        || !database.setMutationState(firstQueuedId, QStringLiteral("failed"), QStringLiteral("stop queue"))
        || !database.nextPendingMutation(accountId).object().isEmpty()
        || !database.retryMutation(firstQueuedId)
        || database.nextPendingMutation(accountId).object().value("id") != firstQueuedId)
        return 34;
    if (!database.discardMutation(firstQueuedId)) return 35;
    const QString secondQueuedId = database.nextPendingMutation(accountId).object().value("id").toString();
    if (secondQueuedId.isEmpty() || !database.discardMutation(secondQueuedId)
        || !database.pendingMutations().array().isEmpty()) return 36;
    if (!database.exportCompatibilityFeed(exportedFeed)) return 37;
    if (!database.removeAccount(accountId))
        return 14;
    if (!database.accounts().array().isEmpty()
        || !database.calendarsForAccount(accountId).array().isEmpty()
        || database.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21")).array().size() != 3
        || database.calendars().array().size() != 1)
        return 15;
    const auto disconnectedEvents = database.eventsForRange(
        QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21"));
    if (!database.importCompatibilityFeed(exportedFeed)
        || database.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21"))
            != disconnectedEvents)
        return 38;
    Database freshDatabase(temporary.filePath(QStringLiteral("fresh.db")));
    if (!freshDatabase.open() || !freshDatabase.importCompatibilityFeed(exportedFeed)
        || !freshDatabase.calendars().array().isEmpty()
        || !freshDatabase.eventsForRange(QStringLiteral("2026-09-20"), QStringLiteral("2026-09-21"))
                .array().isEmpty())
        return 39;
    return 0;
}
