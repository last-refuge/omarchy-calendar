#include "database.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QTimeZone>
#include <QUrl>

namespace {
QJsonArray meetingLinks(const QJsonObject &raw)
{
    QJsonArray result;
    QSet<QString> seen;
    const auto append = [&](const QString &uri, const QString &kind, const QString &label,
                            const QString &details = QString()) {
        const QString normalized = uri.trimmed();
        if (normalized.isEmpty() || seen.contains(normalized)) return;
        seen.insert(normalized);
        QJsonObject item {
            { QStringLiteral("uri"), normalized },
            { QStringLiteral("kind"), kind },
            { QStringLiteral("label"), label }
        };
        if (!details.isEmpty()) item.insert(QStringLiteral("details"), details);
        result.append(item);
    };

    append(raw.value(QStringLiteral("hangoutLink")).toString(),
           QStringLiteral("google-meet"), QStringLiteral("Join Google Meet"));
    for (const QJsonValue &value : raw.value(QStringLiteral("conferenceData")).toObject()
                                      .value(QStringLiteral("entryPoints")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString uri = entry.value(QStringLiteral("uri")).toString();
        const QString type = entry.value(QStringLiteral("entryPointType")).toString();
        QStringList details;
        const QStringList detailFields { QStringLiteral("pin"), QStringLiteral("accessCode"),
                                         QStringLiteral("meetingCode"), QStringLiteral("passcode") };
        for (const QString &field : detailFields) {
            const QString detail = entry.value(field).toString();
            if (!detail.isEmpty()) details.append(field == QStringLiteral("pin")
                ? QStringLiteral("PIN %1").arg(detail)
                : QStringLiteral("Code %1").arg(detail));
        }
        if (type == QStringLiteral("phone")) {
            const QString phoneLabel = entry.value(QStringLiteral("label")).toString();
            append(uri, QStringLiteral("phone"), phoneLabel.isEmpty()
                       ? QStringLiteral("Call meeting") : QStringLiteral("Call %1").arg(phoneLabel),
                   details.join(QStringLiteral(" · ")));
        } else if (type == QStringLiteral("sip")) {
            append(uri, QStringLiteral("sip"), QStringLiteral("Join by SIP"),
                   details.join(QStringLiteral(" · ")));
        } else if (type == QStringLiteral("more")) {
            append(uri, QStringLiteral("more"), QStringLiteral("More joining options"),
                   details.join(QStringLiteral(" · ")));
        } else {
            const QString host = QUrl(uri).host().toLower();
            const QString kind = host.contains(QStringLiteral("zoom.")) ? QStringLiteral("zoom")
                : host.contains(QStringLiteral("teams.")) ? QStringLiteral("teams")
                : host == QStringLiteral("meet.google.com") ? QStringLiteral("google-meet")
                : QStringLiteral("video");
            const QString label = kind == QStringLiteral("zoom") ? QStringLiteral("Join Zoom")
                : kind == QStringLiteral("teams") ? QStringLiteral("Join Microsoft Teams")
                : kind == QStringLiteral("google-meet") ? QStringLiteral("Join Google Meet")
                : QStringLiteral("Join meeting");
            append(uri, kind, label, details.join(QStringLiteral(" · ")));
        }
    }

    static const QRegularExpression urlPattern(QStringLiteral("https?://[^\\s<>\\\"']+"),
                                        QRegularExpression::CaseInsensitiveOption);
    const QString searchable = raw.value(QStringLiteral("description")).toString() + QLatin1Char(' ')
        + raw.value(QStringLiteral("location")).toString();
    auto urls = urlPattern.globalMatch(searchable);
    while (urls.hasNext()) {
        QString uri = urls.next().captured();
        while (!uri.isEmpty() && QStringLiteral(".,;:!?)").contains(uri.back())) uri.chop(1);
        const QString host = QUrl(uri).host().toLower();
        if (host == QStringLiteral("meet.google.com"))
            append(uri, QStringLiteral("google-meet"), QStringLiteral("Join Google Meet"));
        else if (host.contains(QStringLiteral("zoom.")))
            append(uri, QStringLiteral("zoom"), QStringLiteral("Join Zoom"));
        else if (host == QStringLiteral("teams.microsoft.com") || host == QStringLiteral("teams.live.com"))
            append(uri, QStringLiteral("teams"), QStringLiteral("Join Microsoft Teams"));
    }
    return result;
}

QJsonObject eventFromQuery(const QSqlQuery &query)
{
    const QJsonObject raw = QJsonDocument::fromJson(
        query.value(QStringLiteral("display_json")).toByteArray()).object();
    const QJsonObject organizer = raw.value(QStringLiteral("organizer")).toObject();
    const QJsonObject creator = raw.value(QStringLiteral("creator")).toObject();
    const QJsonArray attendees = raw.value(QStringLiteral("attendees")).toArray();
    const QJsonObject reminders = raw.contains(QStringLiteral("reminders"))
        ? raw.value(QStringLiteral("reminders")).toObject()
        : QJsonObject { { QStringLiteral("useDefault"), true } };
    const bool guestsCanModify = raw.value(QStringLiteral("guestsCanModify")).toBool(false);
    const bool recurring = !query.value(QStringLiteral("recurring_event_id")).toString().isEmpty()
        || !query.value(QStringLiteral("recurrence_json")).toString().isEmpty();
    const bool canModify = !raw.value(QStringLiteral("locked")).toBool(false)
        && (organizer.isEmpty() || organizer.value(QStringLiteral("self")).toBool(false)
            || guestsCanModify);
    const QString eventType = raw.value(QStringLiteral("eventType")).toString(QStringLiteral("default"));
    QString selfResponse;
    bool hasSelfAttendee = false;
    for (const QJsonValue &value : attendees) {
        const QJsonObject attendee = value.toObject();
        if (attendee.value(QStringLiteral("self")).toBool()) {
            hasSelfAttendee = true;
            selfResponse = attendee.value(QStringLiteral("responseStatus")).toString();
            break;
        }
    }
    const QString storedTransparency = query.value(QStringLiteral("transparency")).toString();
    QJsonObject result {
        { QStringLiteral("id"), query.value(QStringLiteral("provider_event_id")).toString() },
        { QStringLiteral("calendarId"), query.value(QStringLiteral("calendar_id")).toString() },
        { QStringLiteral("calendarName"), query.value(QStringLiteral("calendar_name")).toString() },
        { QStringLiteral("color"), query.value(QStringLiteral("color")).toString() },
        { QStringLiteral("dateKey"), query.value(QStringLiteral("date_key")).toString() },
        { QStringLiteral("startMs"), query.value(QStringLiteral("start_ms")).toDouble() },
        { QStringLiteral("endMs"), query.value(QStringLiteral("end_ms")).toDouble() },
        { QStringLiteral("allDay"), query.value(QStringLiteral("all_day")).toBool() },
        { QStringLiteral("allDayStartDate"), query.value(QStringLiteral("all_day_start_date")).toString() },
        { QStringLiteral("allDayEndDate"), query.value(QStringLiteral("all_day_end_date")).toString() },
        { QStringLiteral("title"), query.value(QStringLiteral("title")).toString() },
        { QStringLiteral("description"), query.value(QStringLiteral("description")).toString() },
        { QStringLiteral("location"), query.value(QStringLiteral("location")).toString() },
        { QStringLiteral("eventUrl"), query.value(QStringLiteral("event_url")).toString() },
        { QStringLiteral("timeZone"), query.value(QStringLiteral("time_zone")).toString() },
        { QStringLiteral("seriesId"), query.value(QStringLiteral("recurring_event_id")).toString() },
        { QStringLiteral("originalStartMs"), query.value(QStringLiteral("original_start_ms")).toDouble() },
        { QStringLiteral("originalStartDate"), query.value(QStringLiteral("original_start_date")).toString() },
        { QStringLiteral("recurrence"), query.value(QStringLiteral("recurrence_json")).toString() },
        { QStringLiteral("isException"), query.value(QStringLiteral("is_exception")).toBool() },
        { QStringLiteral("isSeriesMaster"), !query.value(QStringLiteral("recurrence_json")).toString().isEmpty()
            && query.value(QStringLiteral("original_start_ms")).toLongLong() == 0
            && query.value(QStringLiteral("original_start_date")).toString().isEmpty() },
        { QStringLiteral("isRecurring"), recurring },
        { QStringLiteral("etag"), query.value(QStringLiteral("etag")).toString() },
        { QStringLiteral("source"), query.value(QStringLiteral("source")).toString() },
        { QStringLiteral("multiDay"), query.value(QStringLiteral("day_count")).toInt() > 1 },
        { QStringLiteral("attendees"), attendees },
        { QStringLiteral("organizer"), organizer },
        { QStringLiteral("creator"), creator },
        { QStringLiteral("reminders"), reminders },
        { QStringLiteral("visibility"), raw.value(QStringLiteral("visibility")).toString(QStringLiteral("default")) },
        { QStringLiteral("transparency"), raw.value(QStringLiteral("transparency")).toString(
            storedTransparency.isEmpty() ? QStringLiteral("opaque") : storedTransparency) },
        { QStringLiteral("guestsCanInviteOthers"), raw.value(QStringLiteral("guestsCanInviteOthers")).toBool(true) },
        { QStringLiteral("guestsCanModify"), guestsCanModify },
        { QStringLiteral("guestsCanSeeOtherGuests"), raw.value(QStringLiteral("guestsCanSeeOtherGuests")).toBool(true) },
        { QStringLiteral("hangoutLink"), raw.value(QStringLiteral("hangoutLink")).toString() },
        { QStringLiteral("conferenceData"), raw.value(QStringLiteral("conferenceData")) },
        { QStringLiteral("meetingLinks"), meetingLinks(raw) },
        { QStringLiteral("attachments"), raw.value(QStringLiteral("attachments")) },
        { QStringLiteral("eventType"), eventType },
        { QStringLiteral("locked"), raw.value(QStringLiteral("locked")).toBool(false) },
        { QStringLiteral("canModify"), canModify },
        { QStringLiteral("canMove"), canModify && !recurring
            && (eventType.isEmpty() || eventType == QStringLiteral("default"))
            && (organizer.isEmpty() || organizer.value(QStringLiteral("self")).toBool(false)) },
        { QStringLiteral("canRespond"), hasSelfAttendee
            && !organizer.value(QStringLiteral("self")).toBool(false) },
        { QStringLiteral("selfResponseStatus"), selfResponse }
    };
    return result;
}

QString googleCalendarId(const QString &accountId, const QString &providerCalendarId)
{
    const QByteArray identity = accountId.toUtf8() + '\0' + providerCalendarId.toUtf8();
    return QStringLiteral("google:")
        + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(32));
}

QDateTime googleDateTime(const QJsonObject &value, const QString &fallbackTimeZone)
{
    const QString dateTime = value.value(QStringLiteral("dateTime")).toString();
    if (!dateTime.isEmpty())
        return QDateTime::fromString(dateTime, Qt::ISODate);

    const QDate date = QDate::fromString(value.value(QStringLiteral("date")).toString(), Qt::ISODate);
    const QByteArray zoneName = value.value(QStringLiteral("timeZone")).toString(fallbackTimeZone).toUtf8();
    const QTimeZone zone(zoneName);
    return QDateTime(date, QTime(0, 0), zone.isValid() ? zone : QTimeZone::systemTimeZone());
}
}

Database::Database(QString path)
    : m_path(std::move(path))
    , m_connectionName(QStringLiteral("omarchy-calendar-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

Database::~Database()
{
    if (m_database.isValid())
        m_database.close();
    m_database = {};
    QSqlDatabase::removeDatabase(m_connectionName);
}

void Database::setError(const QString &context, const QString &detail)
{
    m_lastError = context + QStringLiteral(": ") + detail;
}

bool Database::execute(const QString &statement)
{
    QSqlQuery query(m_database);
    if (query.exec(statement))
        return true;
    setError(QStringLiteral("Database statement failed"), query.lastError().text());
    return false;
}

bool Database::open()
{
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        setError(QStringLiteral("Database directory could not be created"), QFileInfo(m_path).absolutePath());
        return false;
    }

    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(m_path);
    if (!m_database.open()) {
        setError(QStringLiteral("Database could not be opened"), m_database.lastError().text());
        return false;
    }

    return execute(QStringLiteral("PRAGMA journal_mode=WAL"))
        && execute(QStringLiteral("PRAGMA foreign_keys=ON"))
        && execute(QStringLiteral("PRAGMA synchronous=NORMAL"))
        && migrate();
}

bool Database::migrate()
{
    if (!m_database.transaction()) {
        setError(QStringLiteral("Migration transaction could not start"), m_database.lastError().text());
        return false;
    }

    const QStringList statements {
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS calendars ("
            "id TEXT PRIMARY KEY, name TEXT NOT NULL, color TEXT NOT NULL, source TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS events ("
            "provider_event_id TEXT NOT NULL, date_key TEXT NOT NULL, calendar_id TEXT NOT NULL, "
            "start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, all_day INTEGER NOT NULL DEFAULT 0, "
            "title TEXT NOT NULL DEFAULT '', location TEXT NOT NULL DEFAULT '', event_url TEXT NOT NULL DEFAULT '', "
            "source TEXT NOT NULL, PRIMARY KEY(provider_event_id, date_key), "
            "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS events_date_key_idx ON events(date_key, start_ms)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS events_end_ms_idx ON events(end_ms)"),
        QStringLiteral(
            "INSERT OR IGNORE INTO schema_migrations(version, applied_at) "
            "VALUES(1, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
    };

    for (const auto &statement : statements) {
        if (!execute(statement)) {
            m_database.rollback();
            return false;
        }
    }

    QSqlQuery versionTwo(m_database);
    if (!versionTwo.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=2"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionTwo.lastError().text());
        m_database.rollback();
        return false;
    }

    if (!versionTwo.next()) {
        const QStringList versionTwoStatements {
            QStringLiteral(
                "CREATE TABLE accounts ("
                "id TEXT PRIMARY KEY, provider TEXT NOT NULL, provider_account_id TEXT NOT NULL, "
                "display_name TEXT NOT NULL DEFAULT '', email TEXT NOT NULL DEFAULT '', "
                "enabled INTEGER NOT NULL DEFAULT 1, sync_state TEXT NOT NULL DEFAULT 'disconnected', "
                "last_sync_at TEXT NOT NULL DEFAULT '', last_error TEXT NOT NULL DEFAULT '', "
                "created_at TEXT NOT NULL, updated_at TEXT NOT NULL, "
                "UNIQUE(provider, provider_account_id))"),
            QStringLiteral("ALTER TABLE calendars ADD COLUMN account_id TEXT REFERENCES accounts(id) ON DELETE CASCADE"),
            QStringLiteral("ALTER TABLE calendars ADD COLUMN provider_calendar_id TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE calendars ADD COLUMN time_zone TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE calendars ADD COLUMN access_role TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE calendars ADD COLUMN selected INTEGER NOT NULL DEFAULT 1"),
            QStringLiteral(
                "CREATE UNIQUE INDEX calendars_provider_identity_idx "
                "ON calendars(account_id, provider_calendar_id) WHERE account_id IS NOT NULL"),
            QStringLiteral("DROP INDEX events_date_key_idx"),
            QStringLiteral("DROP INDEX events_end_ms_idx"),
            QStringLiteral("ALTER TABLE events RENAME TO events_v1"),
            QStringLiteral(
                "CREATE TABLE events ("
                "provider_event_id TEXT NOT NULL, date_key TEXT NOT NULL, calendar_id TEXT NOT NULL, "
                "start_ms INTEGER NOT NULL, end_ms INTEGER NOT NULL, all_day INTEGER NOT NULL DEFAULT 0, "
                "title TEXT NOT NULL DEFAULT '', description TEXT NOT NULL DEFAULT '', "
                "location TEXT NOT NULL DEFAULT '', event_url TEXT NOT NULL DEFAULT '', "
                "provider_uid TEXT NOT NULL DEFAULT '', time_zone TEXT NOT NULL DEFAULT '', "
                "status TEXT NOT NULL DEFAULT '', transparency TEXT NOT NULL DEFAULT '', "
                "etag TEXT NOT NULL DEFAULT '', provider_updated_at TEXT NOT NULL DEFAULT '', "
                "raw_json TEXT NOT NULL DEFAULT '', source TEXT NOT NULL, "
                "PRIMARY KEY(calendar_id, provider_event_id, date_key), "
                "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE)"),
            QStringLiteral(
                "INSERT INTO events(provider_event_id, date_key, calendar_id, start_ms, end_ms, all_day, "
                "title, location, event_url, source) "
                "SELECT provider_event_id, date_key, calendar_id, start_ms, end_ms, all_day, "
                "title, location, event_url, source FROM events_v1"),
            QStringLiteral("DROP TABLE events_v1"),
            QStringLiteral("CREATE INDEX events_date_key_idx ON events(date_key, start_ms)"),
            QStringLiteral("CREATE INDEX events_end_ms_idx ON events(end_ms)"),
            QStringLiteral(
                "CREATE TABLE sync_cursors ("
                "account_id TEXT NOT NULL, calendar_id TEXT NOT NULL, cursor TEXT NOT NULL DEFAULT '', "
                "updated_at TEXT NOT NULL, PRIMARY KEY(account_id, calendar_id), "
                "FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE, "
                "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE)"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(2, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };

        for (const auto &statement : versionTwoStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionThree(m_database);
    if (!versionThree.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=3"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionThree.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionThree.next()) {
        const QStringList versionThreeStatements {
            QStringLiteral(
                "CREATE TABLE pending_mutations ("
                "id TEXT PRIMARY KEY, account_id TEXT NOT NULL, calendar_id TEXT NOT NULL, "
                "provider_event_id TEXT NOT NULL, operation TEXT NOT NULL, payload_json TEXT NOT NULL, "
                "base_etag TEXT NOT NULL DEFAULT '', state TEXT NOT NULL DEFAULT 'queued', "
                "attempt_count INTEGER NOT NULL DEFAULT 0, last_error TEXT NOT NULL DEFAULT '', "
                "created_at TEXT NOT NULL, updated_at TEXT NOT NULL, "
                "FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE, "
                "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE)"),
            QStringLiteral("CREATE INDEX pending_mutations_state_idx ON pending_mutations(state, created_at)"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(3, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionThreeStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionFour(m_database);
    if (!versionFour.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=4"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionFour.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionFour.next()) {
        const QStringList versionFourStatements {
            QStringLiteral("ALTER TABLE accounts ADD COLUMN granted_scopes TEXT NOT NULL DEFAULT ''"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(4, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionFourStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionFive(m_database);
    if (!versionFive.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=5"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionFive.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionFive.next()) {
        const QStringList versionFiveStatements {
            QStringLiteral("ALTER TABLE events ADD COLUMN all_day_start_date TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE events ADD COLUMN all_day_end_date TEXT NOT NULL DEFAULT ''"),
            QStringLiteral(
                "UPDATE events SET all_day_start_date=COALESCE(NULLIF(CASE WHEN json_valid(raw_json) "
                "THEN json_extract(raw_json,'$.start.date') END,''),date_key), "
                "all_day_end_date=COALESCE(NULLIF(CASE WHEN json_valid(raw_json) "
                "THEN json_extract(raw_json,'$.end.date') END,''),date(date_key,'+1 day')) "
                "WHERE all_day=1"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(5, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionFiveStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionSix(m_database);
    if (!versionSix.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=6"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionSix.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionSix.next()) {
        const QStringList versionSixStatements {
            QStringLiteral(
                "DELETE FROM events WHERE all_day=0 AND rowid NOT IN ("
                "SELECT MIN(rowid) FROM events WHERE all_day=0 GROUP BY calendar_id,provider_event_id)"),
            QStringLiteral(
                "UPDATE events SET date_key=date(start_ms / 1000,'unixepoch','localtime') WHERE all_day=0"),
            QStringLiteral(
                "WITH RECURSIVE days(calendar_id,provider_event_id,date_key,last_date) AS ("
                "SELECT calendar_id,provider_event_id,date(date_key,'+1 day'),"
                "date((end_ms - 1) / 1000,'unixepoch','localtime') FROM events WHERE all_day=0 "
                "UNION ALL SELECT calendar_id,provider_event_id,date(date_key,'+1 day'),last_date "
                "FROM days WHERE date_key<last_date) "
                "INSERT INTO events(provider_event_id,date_key,calendar_id,start_ms,end_ms,all_day,title,"
                "description,location,event_url,provider_uid,time_zone,status,transparency,etag,"
                "provider_updated_at,raw_json,source,all_day_start_date,all_day_end_date) "
                "SELECT e.provider_event_id,d.date_key,e.calendar_id,e.start_ms,e.end_ms,e.all_day,e.title,"
                "e.description,e.location,e.event_url,e.provider_uid,e.time_zone,e.status,e.transparency,e.etag,"
                "e.provider_updated_at,e.raw_json,e.source,e.all_day_start_date,e.all_day_end_date "
                "FROM days d JOIN events e ON e.calendar_id=d.calendar_id "
                "AND e.provider_event_id=d.provider_event_id AND e.all_day=0 WHERE d.date_key<=d.last_date"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(6, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionSixStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionSeven(m_database);
    if (!versionSeven.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=7"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionSeven.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionSeven.next()) {
        const QStringList versionSevenStatements {
            QStringLiteral("ALTER TABLE events ADD COLUMN recurring_event_id TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE events ADD COLUMN original_start_ms INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("ALTER TABLE events ADD COLUMN original_start_date TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE events ADD COLUMN recurrence_json TEXT NOT NULL DEFAULT ''"),
            QStringLiteral("ALTER TABLE events ADD COLUMN is_exception INTEGER NOT NULL DEFAULT 0"),
            QStringLiteral("CREATE INDEX events_recurring_event_idx ON events(calendar_id,recurring_event_id)"),
            QStringLiteral(
                "UPDATE events SET recurring_event_id=COALESCE(CASE WHEN json_valid(raw_json) "
                "THEN json_extract(raw_json,'$.recurringEventId') END,''), "
                "original_start_ms=COALESCE(CASE WHEN json_valid(raw_json) "
                "THEN CAST(strftime('%s',json_extract(raw_json,'$.originalStartTime.dateTime')) AS INTEGER)*1000 END,0), "
                "original_start_date=COALESCE(CASE WHEN json_valid(raw_json) "
                "THEN json_extract(raw_json,'$.originalStartTime.date') END,''), "
                "recurrence_json=COALESCE(CASE WHEN json_valid(raw_json) "
                "THEN json_extract(raw_json,'$.recurrence') END,''), "
                "is_exception=CASE WHEN json_valid(raw_json) AND json_extract(raw_json,'$.recurringEventId') IS NOT NULL "
                "AND ((json_extract(raw_json,'$.originalStartTime.date') IS NOT NULL "
                "AND json_extract(raw_json,'$.originalStartTime.date')!=json_extract(raw_json,'$.start.date')) "
                "OR (json_extract(raw_json,'$.originalStartTime.dateTime') IS NOT NULL "
                "AND CAST(strftime('%s',json_extract(raw_json,'$.originalStartTime.dateTime')) AS INTEGER)*1000!=start_ms)) "
                "THEN 1 ELSE 0 END"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(7, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionSevenStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionEight(m_database);
    if (!versionEight.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=8"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionEight.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionEight.next()) {
        const QStringList versionEightStatements {
            QStringLiteral(
                "CREATE TABLE invitation_notifications (calendar_id TEXT NOT NULL,event_id TEXT NOT NULL,"
                "response_status TEXT NOT NULL,notified_at TEXT NOT NULL,PRIMARY KEY(calendar_id,event_id),"
                "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE)"),
            QStringLiteral(
                "INSERT OR IGNORE INTO invitation_notifications(calendar_id,event_id,response_status,notified_at) "
                "SELECT calendar_id,provider_event_id,'needsAction',strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                "FROM events e WHERE json_valid(e.raw_json) AND EXISTS (SELECT 1 FROM json_each(e.raw_json,'$.attendees') "
                "WHERE json_extract(value,'$.self')=1 AND json_extract(value,'$.responseStatus')='needsAction')"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(8, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionEightStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionNine(m_database);
    if (!versionNine.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=9"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionNine.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionNine.next()) {
        const QStringList versionNineStatements {
            QStringLiteral("ALTER TABLE calendars ADD COLUMN allowed_conference_types TEXT NOT NULL DEFAULT '[]'"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(9, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionNineStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery versionTen(m_database);
    if (!versionTen.exec(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version=10"))) {
        setError(QStringLiteral("Migration version could not be checked"), versionTen.lastError().text());
        m_database.rollback();
        return false;
    }
    if (!versionTen.next()) {
        const QStringList versionTenStatements {
            QStringLiteral("ALTER TABLE calendars ADD COLUMN default_reminders TEXT NOT NULL DEFAULT '[]'"),
            QStringLiteral(
                "CREATE TABLE reminder_deliveries (id TEXT PRIMARY KEY,calendar_id TEXT NOT NULL,"
                "event_id TEXT NOT NULL,start_ms INTEGER NOT NULL,minutes INTEGER NOT NULL,"
                "scheduled_ms INTEGER NOT NULL,state TEXT NOT NULL DEFAULT 'pending',"
                "notification_id INTEGER NOT NULL DEFAULT 0,updated_at TEXT NOT NULL,"
                "FOREIGN KEY(calendar_id) REFERENCES calendars(id) ON DELETE CASCADE)"),
            QStringLiteral("CREATE INDEX reminder_deliveries_due_idx ON reminder_deliveries(state,scheduled_ms)"),
            QStringLiteral(
                "INSERT OR REPLACE INTO metadata(key,value) VALUES('reminder_scheduler_initialized_ms',"
                "CAST(strftime('%s','now') AS INTEGER)*1000)"),
            QStringLiteral(
                "INSERT INTO schema_migrations(version, applied_at) "
                "VALUES(10, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))")
        };
        for (const auto &statement : versionTenStatements) {
            if (!execute(statement)) {
                m_database.rollback();
                return false;
            }
        }
    }

    if (!m_database.commit()) {
        setError(QStringLiteral("Migration transaction could not commit"), m_database.lastError().text());
        return false;
    }
    return true;
}

bool Database::importCompatibilityFeed(const QString &feedPath)
{
    QFile file(feedPath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(QStringLiteral("Compatibility feed could not be opened"), file.errorString());
        return false;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(QStringLiteral("Compatibility feed is invalid"), parseError.errorString());
        return false;
    }

    const auto root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != 1 || !root.value(QStringLiteral("events")).isArray()) {
        setError(QStringLiteral("Compatibility feed is unsupported"), QStringLiteral("expected version 1"));
        return false;
    }

    if (root.value(QStringLiteral("source")).toString() == QStringLiteral("Omarchy Calendar")) {
        QSqlQuery account(QStringLiteral("SELECT 1 FROM accounts WHERE provider='google' LIMIT 1"), m_database);
        if (account.next()) {
            if (!m_database.transaction()) {
                setError(QStringLiteral("Native calendar repair transaction could not start"), m_database.lastError().text());
                return false;
            }
            if (!execute(QStringLiteral(
                    "UPDATE calendars SET source='google' "
                    "WHERE source='compat-json' AND account_id IS NOT NULL"))
                || !execute(QStringLiteral(
                    "UPDATE events SET source='google' WHERE source='compat-json' AND calendar_id IN ("
                    "SELECT id FROM calendars WHERE source='google' AND account_id IS NOT NULL)"))) {
                m_database.rollback();
                return false;
            }
            if (!m_database.commit()) {
                setError(QStringLiteral("Native calendar repair transaction could not commit"), m_database.lastError().text());
                return false;
            }
            m_lastError.clear();
            return true;
        }
        // Our feed is a projection of the database, not a backup to restore.
        // In particular, a stale export must not resurrect disconnected accounts.
        m_lastError.clear();
        return true;
    }

    if (!m_database.transaction()) {
        setError(QStringLiteral("Import transaction could not start"), m_database.lastError().text());
        return false;
    }

    QSqlQuery removeEvents(m_database);
    removeEvents.prepare(QStringLiteral("DELETE FROM events WHERE source = ?"));
    removeEvents.addBindValue(QStringLiteral("compat-json"));
    if (!removeEvents.exec()) {
        setError(QStringLiteral("Old compatibility events could not be replaced"), removeEvents.lastError().text());
        m_database.rollback();
        return false;
    }

    QSqlQuery removeCalendars(m_database);
    removeCalendars.prepare(QStringLiteral("DELETE FROM calendars WHERE source = ?"));
    removeCalendars.addBindValue(QStringLiteral("compat-json"));
    if (!removeCalendars.exec()) {
        setError(QStringLiteral("Old compatibility calendars could not be replaced"), removeCalendars.lastError().text());
        m_database.rollback();
        return false;
    }

    QSqlQuery calendar(m_database);
    calendar.prepare(QStringLiteral(
        "INSERT INTO calendars(id, name, color, source) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET name=excluded.name, color=excluded.color, source=excluded.source"));

    QSqlQuery event(m_database);
    event.prepare(QStringLiteral(
        "INSERT INTO events(provider_event_id, date_key, calendar_id, start_ms, end_ms, all_day, "
        "title, location, event_url, source) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(calendar_id, provider_event_id, date_key) DO UPDATE SET "
        "calendar_id=excluded.calendar_id, start_ms=excluded.start_ms, end_ms=excluded.end_ms, "
        "all_day=excluded.all_day, title=excluded.title, location=excluded.location, "
        "event_url=excluded.event_url, source=excluded.source"));

    for (const auto &value : root.value(QStringLiteral("events")).toArray()) {
        if (!value.isObject())
            continue;
        const auto item = value.toObject();
        const QString calendarId = item.value(QStringLiteral("calendarId")).toString();

        calendar.bindValue(0, calendarId);
        calendar.bindValue(1, item.value(QStringLiteral("calendarName")).toString());
        calendar.bindValue(2, item.value(QStringLiteral("color")).toString());
        calendar.bindValue(3, QStringLiteral("compat-json"));
        if (!calendar.exec()) {
            setError(QStringLiteral("Calendar import failed"), calendar.lastError().text());
            m_database.rollback();
            return false;
        }

        const auto start = QDateTime::fromString(item.value(QStringLiteral("start")).toString(), Qt::ISODate);
        const auto end = QDateTime::fromString(item.value(QStringLiteral("end")).toString(), Qt::ISODate);
        event.bindValue(0, item.value(QStringLiteral("id")).toString());
        event.bindValue(1, item.value(QStringLiteral("dateKey")).toString());
        event.bindValue(2, calendarId);
        event.bindValue(3, start.isValid() ? start.toMSecsSinceEpoch() : 0);
        event.bindValue(4, end.isValid() ? end.toMSecsSinceEpoch() : 0);
        event.bindValue(5, item.value(QStringLiteral("allDay")).toBool() ? 1 : 0);
        event.bindValue(6, item.value(QStringLiteral("title")).toString());
        event.bindValue(7, item.value(QStringLiteral("location")).toString());
        event.bindValue(8, item.value(QStringLiteral("eventUrl")).toString());
        event.bindValue(9, QStringLiteral("compat-json"));
        if (!event.exec()) {
            setError(QStringLiteral("Event import failed"), event.lastError().text());
            m_database.rollback();
            return false;
        }
    }

    QSqlQuery metadata(m_database);
    metadata.prepare(QStringLiteral(
        "INSERT INTO metadata(key, value) VALUES(?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    const QList<QPair<QString, QString>> values {
        { QStringLiteral("compat_feed_synced_at"), root.value(QStringLiteral("syncedAt")).toString() },
        { QStringLiteral("compat_feed_imported_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) }
    };
    for (const auto &[key, value] : values) {
        metadata.bindValue(0, key);
        metadata.bindValue(1, value);
        if (!metadata.exec()) {
            setError(QStringLiteral("Import metadata failed"), metadata.lastError().text());
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        setError(QStringLiteral("Import transaction could not commit"), m_database.lastError().text());
        return false;
    }

    m_lastError.clear();
    return true;
}

bool Database::exportCompatibilityFeed(const QString &feedPath)
{
    QJsonArray events;
    QSqlQuery query(QStringLiteral(
        "SELECT e.provider_event_id, e.calendar_id, c.name, c.color, e.date_key, "
        "e.start_ms, e.end_ms, e.all_day, e.title, e.location, e.event_url "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE e.source IN ('google','local-pending') AND c.selected=1 "
        "ORDER BY e.date_key, e.start_ms, e.title"), m_database);
    if (query.lastError().isValid()) {
        setError(QStringLiteral("Compatibility export query failed"), query.lastError().text());
        return false;
    }

    while (query.next()) {
        events.append(QJsonObject {
            { QStringLiteral("id"), query.value(0).toString() },
            { QStringLiteral("calendarId"), query.value(1).toString() },
            { QStringLiteral("calendarName"), query.value(2).toString() },
            { QStringLiteral("color"), query.value(3).toString() },
            { QStringLiteral("dateKey"), query.value(4).toString() },
            { QStringLiteral("start"), QDateTime::fromMSecsSinceEpoch(query.value(5).toLongLong(), QTimeZone::UTC)
                                                  .toString(Qt::ISODateWithMs) },
            { QStringLiteral("end"), QDateTime::fromMSecsSinceEpoch(query.value(6).toLongLong(), QTimeZone::UTC)
                                                .toString(Qt::ISODateWithMs) },
            { QStringLiteral("allDay"), query.value(7).toBool() },
            { QStringLiteral("title"), query.value(8).toString() },
            { QStringLiteral("location"), query.value(9).toString() },
            { QStringLiteral("eventUrl"), query.value(10).toString() }
        });
    }

    const QFileInfo outputInfo(feedPath);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        setError(QStringLiteral("Compatibility export directory could not be created"), outputInfo.absolutePath());
        return false;
    }
    QSaveFile output(feedPath);
    if (!output.open(QIODevice::WriteOnly)) {
        setError(QStringLiteral("Compatibility export could not be opened"), output.errorString());
        return false;
    }
    const QJsonDocument document(QJsonObject {
        { QStringLiteral("version"), 1 },
        { QStringLiteral("syncedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
        { QStringLiteral("source"), QStringLiteral("Omarchy Calendar") },
        { QStringLiteral("events"), events }
    });
    if (output.write(document.toJson(QJsonDocument::Indented)) < 0 || !output.commit()) {
        setError(QStringLiteral("Compatibility export could not be committed"), output.errorString());
        return false;
    }
    QFile::setPermissions(feedPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    m_lastError.clear();
    return true;
}

QJsonDocument Database::eventsForRange(const QString &firstDate, const QString &lastDate) const
{
    QJsonArray events;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT e.provider_event_id, e.calendar_id, c.name AS calendar_name, c.color, e.date_key, "
        "e.start_ms, e.end_ms, e.all_day, e.title, e.description, e.location, e.event_url,"
        "COALESCE((SELECT p.payload_json FROM pending_mutations p WHERE p.calendar_id=e.calendar_id "
        "AND p.provider_event_id=e.provider_event_id AND p.operation IN ('update','update-series','update-future','rsvp') "
        "ORDER BY p.created_at DESC LIMIT 1),e.raw_json) AS display_json,e.transparency, "
        "COALESCE(NULLIF(e.time_zone,''),c.time_zone) AS time_zone, e.etag, e.source,e.all_day_start_date,e.all_day_end_date, "
        "e.recurring_event_id,e.original_start_ms,e.original_start_date,e.recurrence_json,e.is_exception, "
        "(SELECT COUNT(*) FROM events span WHERE span.calendar_id=e.calendar_id "
        "AND span.provider_event_id=e.provider_event_id) AS day_count "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE e.date_key BETWEEN ? AND ? AND c.selected=1 "
        "AND (c.source!='compat-json' OR NOT EXISTS ("
        "SELECT 1 FROM accounts WHERE provider='google' AND last_sync_at!='')) "
        "ORDER BY e.start_ms, e.title"));
    query.addBindValue(firstDate);
    query.addBindValue(lastDate);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return QJsonDocument(events);
    }
    while (query.next())
        events.append(eventFromQuery(query));
    return QJsonDocument(events);
}

QJsonDocument Database::calendars() const
{
    QJsonArray calendars;
    QSqlQuery query(QStringLiteral(
        "SELECT id, name, color, source, account_id, provider_calendar_id, "
        "time_zone, access_role, selected, allowed_conference_types FROM calendars "
        "WHERE source!='compat-json' OR NOT EXISTS ("
        "SELECT 1 FROM accounts WHERE provider='google' AND last_sync_at!='') "
        "ORDER BY name"), m_database);
    while (query.next()) {
        calendars.append(QJsonObject {
            { QStringLiteral("id"), query.value(0).toString() },
            { QStringLiteral("name"), query.value(1).toString() },
            { QStringLiteral("color"), query.value(2).toString() },
            { QStringLiteral("source"), query.value(3).toString() },
            { QStringLiteral("accountId"), query.value(4).toString() },
            { QStringLiteral("providerCalendarId"), query.value(5).toString() },
            { QStringLiteral("timeZone"), query.value(6).toString() },
            { QStringLiteral("accessRole"), query.value(7).toString() },
            { QStringLiteral("selected"), query.value(8).toBool() },
            { QStringLiteral("allowedConferenceTypes"),
              QJsonDocument::fromJson(query.value(9).toByteArray()).array() }
        });
    }
    return QJsonDocument(calendars);
}

QJsonDocument Database::accounts() const
{
    QJsonArray accounts;
    QSqlQuery query(QStringLiteral(
        "SELECT id, provider, provider_account_id, display_name, email, enabled, "
        "sync_state, last_sync_at, last_error FROM accounts ORDER BY display_name, email"), m_database);
    while (query.next()) {
        accounts.append(QJsonObject {
            { QStringLiteral("id"), query.value(0).toString() },
            { QStringLiteral("provider"), query.value(1).toString() },
            { QStringLiteral("providerAccountId"), query.value(2).toString() },
            { QStringLiteral("displayName"), query.value(3).toString() },
            { QStringLiteral("email"), query.value(4).toString() },
            { QStringLiteral("enabled"), query.value(5).toBool() },
            { QStringLiteral("syncState"), query.value(6).toString() },
            { QStringLiteral("lastSyncAt"), query.value(7).toString() },
            { QStringLiteral("lastError"), query.value(8).toString() }
        });
    }
    return QJsonDocument(accounts);
}

bool Database::upsertAccount(const QString &id, const QString &provider,
                             const QString &providerAccountId, const QString &displayName,
                             const QString &email, const QString &syncState)
{
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO accounts(id, provider, provider_account_id, display_name, email, sync_state, created_at, updated_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET "
        "provider_account_id=excluded.provider_account_id, display_name=excluded.display_name, "
        "email=excluded.email, sync_state=excluded.sync_state, updated_at=excluded.updated_at"));
    query.addBindValue(id);
    query.addBindValue(provider);
    query.addBindValue(providerAccountId);
    query.addBindValue(displayName);
    query.addBindValue(email);
    query.addBindValue(syncState);
    query.addBindValue(now);
    query.addBindValue(now);
    if (query.exec())
        return true;
    setError(QStringLiteral("Account could not be saved"), query.lastError().text());
    return false;
}

bool Database::updateAccountSyncState(const QString &id, const QString &syncState,
                                      const QString &lastError)
{
    QSqlQuery query(m_database);
    const bool synced = syncState == QStringLiteral("idle");
    query.prepare(synced
        ? QStringLiteral("UPDATE accounts SET sync_state=?, last_error=?, last_sync_at=?, updated_at=? WHERE id=?")
        : QStringLiteral("UPDATE accounts SET sync_state=?, last_error=?, updated_at=? WHERE id=?"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    query.addBindValue(syncState);
    query.addBindValue(lastError.isNull() ? QStringLiteral("") : lastError);
    if (synced)
        query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(id);
    if (query.exec())
        return true;
    setError(QStringLiteral("Account state could not be updated"), query.lastError().text());
    return false;
}

bool Database::setAccountGrantedScopes(const QString &id, const QString &scopes)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("UPDATE accounts SET granted_scopes=?, updated_at=? WHERE id=?"));
    query.addBindValue(scopes);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(id);
    if (query.exec() && query.numRowsAffected() == 1)
        return true;
    setError(QStringLiteral("Google scopes could not be stored"), query.lastError().text());
    return false;
}

QString Database::accountGrantedScopes(const QString &id) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT granted_scopes FROM accounts WHERE id=?"));
    query.addBindValue(id);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool Database::requeueBlockedMutations(const QString &accountId)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE pending_mutations SET state='queued',last_error='',updated_at=? "
        "WHERE account_id=? AND state='blocked'"));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(accountId);
    if (query.exec()) return true;
    setError(QStringLiteral("Blocked mutations could not be requeued"), query.lastError().text());
    return false;
}

bool Database::removeAccount(const QString &id)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM accounts WHERE id=?"));
    query.addBindValue(id);
    if (query.exec())
        return true;
    setError(QStringLiteral("Account could not be removed"), query.lastError().text());
    return false;
}

bool Database::setCalendarSelected(const QString &calendarId, bool selected)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE calendars SET selected=? WHERE id=? AND source='google'"));
    query.addBindValue(selected ? 1 : 0);
    query.addBindValue(calendarId);
    if (!query.exec()) {
        setError(QStringLiteral("Calendar selection could not be updated"), query.lastError().text());
        return false;
    }
    if (query.numRowsAffected() != 1) {
        setError(QStringLiteral("Calendar selection could not be updated"), QStringLiteral("calendar not found"));
        return false;
    }
    m_lastError.clear();
    return true;
}

QString Database::createPendingEvent(const QJsonObject &event)
{
    const QString calendarId = event.value("calendarId").toString();
    const QString title = event.value("title").toString().trimmed();
    const qint64 startMs = qint64(event.value("startMs").toDouble());
    const qint64 endMs = qint64(event.value("endMs").toDouble());
    if (calendarId.isEmpty() || title.isEmpty() || startMs <= 0 || endMs <= startMs) {
        setError("Event could not be created", "title, calendar, and valid times are required");
        return {};
    }
    QSqlQuery calendar(m_database);
    calendar.prepare("SELECT account_id,time_zone,access_role,allowed_conference_types FROM calendars WHERE id=? AND source='google'");
    calendar.addBindValue(calendarId);
    if (!calendar.exec() || !calendar.next()
        || (calendar.value(2).toString() != "owner" && calendar.value(2).toString() != "writer")) {
        setError("Event could not be created", "calendar is unavailable or read-only");
        return {};
    }
    if (event.value(QStringLiteral("createConference")).toBool()
        && !QJsonDocument::fromJson(calendar.value(3).toByteArray()).array()
                .contains(QStringLiteral("hangoutsMeet"))) {
        setError(QStringLiteral("Event could not be created"),
                 QStringLiteral("this calendar does not support Google Meet creation"));
        return {};
    }
    QTimeZone zone(event.value("timeZone").toString(calendar.value(1).toString()).toUtf8());
    if (!zone.isValid()) zone = QTimeZone::systemTimeZone();
    const bool allDay = event.value("allDay").toBool();
    const QString allDayStart = allDay ? event.value("allDayStartDate").toString() : QStringLiteral("");
    const QString allDayEnd = allDay ? event.value("allDayEndDate").toString() : QStringLiteral("");
    const QJsonArray recurrence = event.value(QStringLiteral("recurrence")).toArray();
    for (const QJsonValue &value : recurrence) {
        if (!value.isString() || !value.toString().startsWith(QStringLiteral("RRULE:"))) {
            setError(QStringLiteral("Event could not be created"), QStringLiteral("recurrence rule is invalid"));
            return {};
        }
    }
    const QString recurrenceJson = recurrence.isEmpty() ? QStringLiteral("")
        : QString::fromUtf8(QJsonDocument(recurrence).toJson(QJsonDocument::Compact));
    const QDate firstDate = allDay ? QDate::fromString(allDayStart, Qt::ISODate)
                                   : QDateTime::fromMSecsSinceEpoch(startMs).date();
    const QDate lastDate = allDay ? QDate::fromString(allDayEnd, Qt::ISODate).addDays(-1)
                                  : QDateTime::fromMSecsSinceEpoch(endMs - 1).date();
    if (!firstDate.isValid() || !lastDate.isValid() || lastDate < firstDate) {
        setError("Event could not be created", "date range is invalid");
        return {};
    }
    const QString eventId = "local:" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QJsonObject payload = event;
    payload.insert("id", eventId);
    payload.insert("googleEventId", QString::fromLatin1(
        QCryptographicHash::hash(eventId.toUtf8(), QCryptographicHash::Sha256).toHex().left(32)));
    payload.insert("timeZone", QString::fromUtf8(zone.id()));
    if (payload.value(QStringLiteral("createConference")).toBool())
        payload.insert(QStringLiteral("conferenceRequestId"),
                       QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString json = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (!m_database.transaction()) return {};
    QSqlQuery row(m_database);
    row.prepare("INSERT INTO events(provider_event_id,date_key,calendar_id,start_ms,end_ms,all_day,title,description,location,time_zone,status,transparency,raw_json,source,all_day_start_date,all_day_end_date,recurring_event_id,recurrence_json) VALUES(?,?,?,?,?,?,?,?,?,?,'confirmed',?,?,'local-pending',?,?,?,?)");
    bool rowsStored = true;
    for (QDate date = firstDate; date <= lastDate; date = date.addDays(1)) {
        int column = 0;
        row.bindValue(column++, eventId);
        row.bindValue(column++, date.toString(Qt::ISODate));
        row.bindValue(column++, calendarId); row.bindValue(column++, startMs); row.bindValue(column++, endMs);
        row.bindValue(column++, allDay ? 1 : 0); row.bindValue(column++, title);
        row.bindValue(column++, event.value("description").toString(QStringLiteral("")));
        row.bindValue(column++, event.value("location").toString(QStringLiteral("")));
        row.bindValue(column++, QString::fromUtf8(zone.id()));
        row.bindValue(column++, event.value(QStringLiteral("transparency")).toString(QStringLiteral("opaque")));
        row.bindValue(column++, json);
        row.bindValue(column++, allDayStart); row.bindValue(column++, allDayEnd);
        row.bindValue(column++, recurrence.isEmpty() ? QStringLiteral("") : eventId);
        row.bindValue(column++, recurrenceJson);
        if (!row.exec()) { rowsStored = false; break; }
    }
    QSqlQuery mutation(m_database);
    mutation.prepare("INSERT INTO pending_mutations(id,account_id,calendar_id,provider_event_id,operation,payload_json,created_at,updated_at) VALUES(?,?,?,?,'create',?,?,?)");
    mutation.addBindValue(mutationId); mutation.addBindValue(calendar.value(0).toString());
    mutation.addBindValue(calendarId); mutation.addBindValue(eventId); mutation.addBindValue(json);
    mutation.addBindValue(now); mutation.addBindValue(now);
    if (!rowsStored || !mutation.exec() || !m_database.commit()) {
        setError("Event could not be queued", row.lastError().text() + mutation.lastError().text());
        m_database.rollback();
        return {};
    }
    m_lastError.clear();
    return eventId;
}

bool Database::updatePendingEvent(const QJsonObject &event)
{
    const QString eventId = event.value(QStringLiteral("id")).toString();
    const QString calendarId = event.value(QStringLiteral("calendarId")).toString();
    const QString title = event.value(QStringLiteral("title")).toString().trimmed();
    const qint64 startMs = qint64(event.value(QStringLiteral("startMs")).toDouble());
    const qint64 endMs = qint64(event.value(QStringLiteral("endMs")).toDouble());
    if (eventId.isEmpty() || calendarId.isEmpty() || title.isEmpty()
        || startMs <= 0 || endMs <= startMs) {
        setError(QStringLiteral("Event could not be updated"),
                 QStringLiteral("event, title, calendar, and valid times are required"));
        return false;
    }

    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral(
        "SELECT c.account_id,c.time_zone,c.access_role,e.etag,e.source,e.event_url,e.provider_uid,"
        "e.status,e.transparency,e.provider_updated_at,e.raw_json,e.recurring_event_id,"
        "e.original_start_ms,e.original_start_date,e.recurrence_json,e.is_exception "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE e.calendar_id=? AND e.provider_event_id=? ORDER BY e.date_key LIMIT 1"));
    existing.addBindValue(calendarId);
    existing.addBindValue(eventId);
    if (!existing.exec() || !existing.next()) {
        setError(QStringLiteral("Event could not be updated"), QStringLiteral("event not found"));
        return false;
    }
    const QString accessRole = existing.value(2).toString();
    if (accessRole != QStringLiteral("owner") && accessRole != QStringLiteral("writer")) {
        setError(QStringLiteral("Event could not be updated"), QStringLiteral("calendar is read-only"));
        return false;
    }

    if (event.value(QStringLiteral("createConference")).toBool()) {
        QSqlQuery conferenceCalendar(m_database);
        conferenceCalendar.prepare(QStringLiteral(
            "SELECT allowed_conference_types FROM calendars WHERE id=? LIMIT 1"));
        conferenceCalendar.addBindValue(event.value(QStringLiteral("targetCalendarId")).toString(calendarId));
        if (!conferenceCalendar.exec() || !conferenceCalendar.next()
            || !QJsonDocument::fromJson(conferenceCalendar.value(0).toByteArray()).array()
                    .contains(QStringLiteral("hangoutsMeet"))) {
            setError(QStringLiteral("Event could not be updated"),
                     QStringLiteral("this calendar does not support Google Meet creation"));
            return false;
        }
    }

    const QJsonObject storedEvent = QJsonDocument::fromJson(existing.value(10).toByteArray()).object();
    const QString targetCalendarId = event.value(QStringLiteral("targetCalendarId")).toString(calendarId);
    const bool calendarMove = targetCalendarId != calendarId;
    QString targetProviderCalendarId;
    if (calendarMove) {
        const QJsonObject organizer = storedEvent.value(QStringLiteral("organizer")).toObject();
        const QString eventType = storedEvent.value(QStringLiteral("eventType")).toString(QStringLiteral("default"));
        if (!existing.value(11).toString().isEmpty() || !existing.value(14).toString().isEmpty()
            || (!eventType.isEmpty() && eventType != QStringLiteral("default"))
            || (!organizer.isEmpty() && !organizer.value(QStringLiteral("self")).toBool(false))) {
            setError(QStringLiteral("Event could not be moved"),
                     QStringLiteral("Google only allows the organizer to move a standard, non-recurring event"));
            return false;
        }
        QSqlQuery target(m_database);
        target.prepare(QStringLiteral(
            "SELECT provider_calendar_id,access_role FROM calendars "
            "WHERE id=? AND account_id=? AND source='google' LIMIT 1"));
        target.addBindValue(targetCalendarId);
        target.addBindValue(existing.value(0).toString());
        if (!target.exec() || !target.next()
            || (target.value(1).toString() != QStringLiteral("owner")
                && target.value(1).toString() != QStringLiteral("writer"))) {
            setError(QStringLiteral("Event could not be moved"),
                     QStringLiteral("destination calendar is unavailable, read-only, or belongs to another account"));
            return false;
        }
        targetProviderCalendarId = target.value(0).toString();
    }

    QTimeZone zone(event.value(QStringLiteral("timeZone")).toString(existing.value(1).toString()).toUtf8());
    if (!zone.isValid()) zone = QTimeZone::systemTimeZone();
    QJsonObject payload = event;
    payload.insert(QStringLiteral("calendarId"), calendarId);
    if (calendarMove) {
        payload.insert(QStringLiteral("targetCalendarId"), targetCalendarId);
        payload.insert(QStringLiteral("targetProviderCalendarId"), targetProviderCalendarId);
    }
    payload.insert(QStringLiteral("timeZone"), QString::fromUtf8(zone.id()));
    if (payload.value(QStringLiteral("createConference")).toBool()
        && payload.value(QStringLiteral("conferenceRequestId")).toString().isEmpty())
        payload.insert(QStringLiteral("conferenceRequestId"),
                       QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QStringList preservedFields {
        QStringLiteral("attendees"), QStringLiteral("reminders"),
        QStringLiteral("visibility"), QStringLiteral("transparency"),
        QStringLiteral("guestsCanInviteOthers"), QStringLiteral("guestsCanModify"),
        QStringLiteral("guestsCanSeeOtherGuests"), QStringLiteral("createConference"),
        QStringLiteral("conferenceRequestId")
    };
    for (const QString &field : preservedFields) {
        if (!payload.contains(field) && storedEvent.contains(field))
            payload.insert(field, storedEvent.value(field));
    }
    const QString editScope = event.value(QStringLiteral("scope")).toString();
    const bool seriesScope = (editScope == QStringLiteral("series") || editScope == QStringLiteral("future"))
        && !event.value(QStringLiteral("seriesId")).toString().isEmpty();
    const QString updateOperation = calendarMove ? QStringLiteral("move")
        : editScope == QStringLiteral("future")
        ? QStringLiteral("update-future")
        : seriesScope ? QStringLiteral("update-series") : QStringLiteral("update");
    if (existing.value(4).toString() == QStringLiteral("local-pending")
        && !payload.contains(QStringLiteral("recurrence"))
        && !existing.value(14).toString().isEmpty()) {
        payload.insert(QStringLiteral("recurrence"),
                       QJsonDocument::fromJson(existing.value(14).toByteArray()).array());
    }
    const bool allDay = event.value(QStringLiteral("allDay")).toBool();
    const QString allDayStart = allDay ? event.value(QStringLiteral("allDayStartDate")).toString() : QStringLiteral("");
    const QString allDayEnd = allDay ? event.value(QStringLiteral("allDayEndDate")).toString() : QStringLiteral("");
    const QDate firstDate = allDay ? QDate::fromString(allDayStart, Qt::ISODate)
                                   : QDateTime::fromMSecsSinceEpoch(startMs).date();
    const QDate lastDate = allDay ? QDate::fromString(allDayEnd, Qt::ISODate).addDays(-1)
                                  : QDateTime::fromMSecsSinceEpoch(endMs - 1).date();
    if (!firstDate.isValid() || !lastDate.isValid() || lastDate < firstDate) {
        setError(QStringLiteral("Event could not be updated"), QStringLiteral("date range is invalid"));
        return false;
    }
    if (existing.value(4).toString() == QStringLiteral("local-pending"))
        payload.insert(QStringLiteral("googleEventId"), QString::fromLatin1(
            QCryptographicHash::hash(eventId.toUtf8(), QCryptographicHash::Sha256).toHex().left(32)));
    const QString json = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    if (!m_database.transaction()) return false;
    QSqlQuery removeRows(m_database);
    removeRows.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND provider_event_id=?"));
    removeRows.addBindValue(calendarId);
    removeRows.addBindValue(eventId);
    if (!removeRows.exec()) { m_database.rollback(); return false; }
    QSqlQuery row(m_database);
    row.prepare(QStringLiteral(
        "INSERT INTO events(provider_event_id,date_key,calendar_id,start_ms,end_ms,all_day,title,description,"
        "location,event_url,provider_uid,time_zone,status,transparency,etag,provider_updated_at,raw_json,source,"
        "all_day_start_date,all_day_end_date,recurring_event_id,original_start_ms,original_start_date,"
        "recurrence_json,is_exception) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    bool rowsStored = true;
    for (QDate date = firstDate; date <= lastDate; date = date.addDays(1)) {
        int column = 0;
        row.bindValue(column++, eventId); row.bindValue(column++, date.toString(Qt::ISODate));
        row.bindValue(column++, calendarMove ? targetCalendarId : calendarId);
        row.bindValue(column++, startMs); row.bindValue(column++, endMs);
        row.bindValue(column++, allDay ? 1 : 0); row.bindValue(column++, title);
        row.bindValue(column++, event.value(QStringLiteral("description")).toString(QStringLiteral("")));
        row.bindValue(column++, event.value(QStringLiteral("location")).toString(QStringLiteral("")));
        row.bindValue(column++, existing.value(5)); row.bindValue(column++, existing.value(6));
        row.bindValue(column++, QString::fromUtf8(zone.id())); row.bindValue(column++, existing.value(7));
        row.bindValue(column++, event.value(QStringLiteral("transparency")).toString(existing.value(8).toString()));
        row.bindValue(column++, existing.value(3));
        row.bindValue(column++, existing.value(9));
        row.bindValue(column++, calendarMove ? json : existing.value(10).toString());
        row.bindValue(column++, calendarMove ? QStringLiteral("local-pending") : existing.value(4));
        row.bindValue(column++, allDayStart); row.bindValue(column++, allDayEnd);
        row.bindValue(column++, existing.value(11)); row.bindValue(column++, existing.value(12));
        row.bindValue(column++, existing.value(13)); row.bindValue(column++, existing.value(14));
        row.bindValue(column++, existing.value(15));
        if (!row.exec()) { rowsStored = false; break; }
    }

    bool queued = false;
    QSqlQuery mutation(m_database);
    if (existing.value(4).toString() == QStringLiteral("local-pending")) {
        mutation.prepare(QStringLiteral(
            "UPDATE pending_mutations SET calendar_id=?,payload_json=?,updated_at=? "
            "WHERE calendar_id=? AND provider_event_id=? AND operation='create'"));
        mutation.addBindValue(calendarMove ? targetCalendarId : calendarId);
        mutation.addBindValue(json);
        mutation.addBindValue(now);
        mutation.addBindValue(calendarId);
        mutation.addBindValue(eventId);
        queued = mutation.exec() && mutation.numRowsAffected() == 1;
    } else {
        mutation.prepare(QStringLiteral(
            "UPDATE pending_mutations SET payload_json=?,base_etag=?,state='queued',last_error='',updated_at=? "
            "WHERE id=(SELECT id FROM pending_mutations WHERE calendar_id=? AND provider_event_id=? "
            "AND operation=? AND state IN ('queued','retrying','failed') ORDER BY created_at DESC LIMIT 1)"));
        mutation.addBindValue(json);
        mutation.addBindValue((seriesScope || calendarMove) ? QStringLiteral("") : existing.value(3).toString());
        mutation.addBindValue(now);
        mutation.addBindValue(calendarId);
        mutation.addBindValue(eventId);
        mutation.addBindValue(updateOperation);
        queued = mutation.exec();
        if (queued && mutation.numRowsAffected() == 0) {
            mutation.prepare(QStringLiteral(
                "INSERT INTO pending_mutations(id,account_id,calendar_id,provider_event_id,operation,payload_json,"
                "base_etag,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?)"));
            mutation.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
            mutation.addBindValue(existing.value(0).toString());
            mutation.addBindValue(calendarId);
            mutation.addBindValue(eventId);
            mutation.addBindValue(updateOperation);
            mutation.addBindValue(json);
            mutation.addBindValue((seriesScope || calendarMove) ? QStringLiteral("") : existing.value(3).toString());
            mutation.addBindValue(now);
            mutation.addBindValue(now);
            queued = mutation.exec();
        }
    }
    if (!rowsStored || !queued || !m_database.commit()) {
        setError(QStringLiteral("Event update could not be queued"),
                 row.lastError().text() + mutation.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::respondPendingEvent(const QString &calendarId, const QString &eventId,
                                   const QString &responseStatus)
{
    const QSet<QString> allowed { QStringLiteral("accepted"), QStringLiteral("declined"),
                                  QStringLiteral("tentative") };
    if (calendarId.isEmpty() || eventId.isEmpty() || !allowed.contains(responseStatus)) {
        setError(QStringLiteral("Invitation response could not be queued"),
                 QStringLiteral("event and a valid response are required"));
        return false;
    }
    QSqlQuery event(m_database);
    event.prepare(QStringLiteral(
        "SELECT c.account_id,c.access_role,e.etag,e.raw_json,e.source FROM events e "
        "JOIN calendars c ON c.id=e.calendar_id WHERE e.calendar_id=? AND e.provider_event_id=? LIMIT 1"));
    event.addBindValue(calendarId);
    event.addBindValue(eventId);
    if (!event.exec() || !event.next()) {
        setError(QStringLiteral("Invitation response could not be queued"), QStringLiteral("event was not found"));
        return false;
    }
    if (event.value(1).toString() != QStringLiteral("owner")
        && event.value(1).toString() != QStringLiteral("writer")) {
        setError(QStringLiteral("Invitation response could not be queued"), QStringLiteral("calendar is read-only"));
        return false;
    }
    if (event.value(4).toString() == QStringLiteral("local-pending")) {
        setError(QStringLiteral("Invitation response could not be queued"),
                 QStringLiteral("the invitation has not synchronized yet"));
        return false;
    }
    QJsonObject payload = QJsonDocument::fromJson(event.value(3).toByteArray()).object();
    QJsonArray attendees = payload.value(QStringLiteral("attendees")).toArray();
    bool foundSelf = false;
    for (qsizetype index = 0; index < attendees.size(); ++index) {
        QJsonObject attendee = attendees.at(index).toObject();
        if (!attendee.value(QStringLiteral("self")).toBool()) continue;
        attendee.insert(QStringLiteral("responseStatus"), responseStatus);
        attendees.replace(index, attendee);
        foundSelf = true;
        break;
    }
    if (!foundSelf || payload.value(QStringLiteral("organizer")).toObject()
                          .value(QStringLiteral("self")).toBool(false)) {
        setError(QStringLiteral("Invitation response could not be queued"),
                 QStringLiteral("this event is not an invitation for the signed-in user"));
        return false;
    }
    payload.insert(QStringLiteral("attendees"), attendees);
    payload.insert(QStringLiteral("responseStatus"), responseStatus);
    payload.insert(QStringLiteral("title"), payload.value(QStringLiteral("summary")));
    const QString json = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery mutation(m_database);
    mutation.prepare(QStringLiteral(
        "UPDATE pending_mutations SET payload_json=?,base_etag=?,state='queued',last_error='',updated_at=? "
        "WHERE id=(SELECT id FROM pending_mutations WHERE calendar_id=? AND provider_event_id=? "
        "AND operation='rsvp' AND state IN ('queued','retrying','failed','conflict','blocked') "
        "ORDER BY created_at DESC LIMIT 1)"));
    mutation.addBindValue(json);
    mutation.addBindValue(event.value(2).toString());
    mutation.addBindValue(now);
    mutation.addBindValue(calendarId);
    mutation.addBindValue(eventId);
    if (!mutation.exec()) return false;
    if (mutation.numRowsAffected() == 0) {
        mutation.prepare(QStringLiteral(
            "INSERT INTO pending_mutations(id,account_id,calendar_id,provider_event_id,operation,payload_json,"
            "base_etag,created_at,updated_at) VALUES(?,?,?,?, 'rsvp',?,?,?,?)"));
        mutation.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        mutation.addBindValue(event.value(0).toString());
        mutation.addBindValue(calendarId);
        mutation.addBindValue(eventId);
        mutation.addBindValue(json);
        mutation.addBindValue(event.value(2).toString());
        mutation.addBindValue(now);
        mutation.addBindValue(now);
        if (!mutation.exec()) {
            setError(QStringLiteral("Invitation response could not be queued"), mutation.lastError().text());
            return false;
        }
    }
    m_lastError.clear();
    return true;
}

QString Database::deletePendingEvent(const QString &calendarId, const QString &eventId)
{
    return deletePendingEvent(QJsonObject {
        { QStringLiteral("calendarId"), calendarId },
        { QStringLiteral("id"), eventId },
        { QStringLiteral("scope"), QStringLiteral("occurrence") }
    });
}

QString Database::deletePendingEvent(const QJsonObject &event)
{
    const QString calendarId = event.value(QStringLiteral("calendarId")).toString();
    const QString eventId = event.value(QStringLiteral("id")).toString();
    if (calendarId.isEmpty() || eventId.isEmpty()) {
        setError(QStringLiteral("Event could not be deleted"), QStringLiteral("event identity is required"));
        return {};
    }
    QSqlQuery selected(m_database);
    selected.prepare(QStringLiteral(
        "SELECT c.account_id,c.access_role,e.source,e.etag,e.recurring_event_id,e.original_start_ms,"
        "e.original_start_date,e.time_zone,e.start_ms,e.date_key,e.all_day "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE e.calendar_id=? AND e.provider_event_id=? LIMIT 1"));
    selected.addBindValue(calendarId);
    selected.addBindValue(eventId);
    if (!selected.exec() || !selected.next()) {
        setError(QStringLiteral("Event could not be deleted"), QStringLiteral("event not found"));
        return {};
    }
    const QString accountId = selected.value(0).toString();
    const QString accessRole = selected.value(1).toString();
    const QString source = selected.value(2).toString();
    const QString etag = selected.value(3).toString();
    const QString selectedTimeZone = selected.value(7).toString();
    const QString seriesId = event.value(QStringLiteral("seriesId")).toString(
        selected.value(4).toString());
    const qint64 originalStartMs = qint64(event.value(QStringLiteral("originalStartMs")).toDouble(
        selected.value(5).toLongLong() > 0 ? selected.value(5).toDouble() : selected.value(8).toDouble()));
    const QString storedOriginalDate = selected.value(6).toString();
    const QString originalStartDate = event.value(QStringLiteral("originalStartDate")).toString(
        storedOriginalDate.isEmpty() && selected.value(10).toBool()
            ? selected.value(9).toString() : storedOriginalDate);
    selected.finish();
    QString scope = event.value(QStringLiteral("scope")).toString(QStringLiteral("occurrence"));
    if ((scope == QStringLiteral("series") || scope == QStringLiteral("future")) && seriesId.isEmpty())
        scope = QStringLiteral("occurrence");
    if (accessRole != QStringLiteral("owner") && accessRole != QStringLiteral("writer")) {
        setError(QStringLiteral("Event could not be deleted"), QStringLiteral("calendar is read-only"));
        return {};
    }
    QSqlQuery query(m_database);
    QString rowSql = QStringLiteral(
        "SELECT e.provider_event_id,e.etag,e.source,e.date_key,e.start_ms,e.end_ms,e.all_day,"
        "e.title,e.description,e.location,e.event_url,e.provider_uid,e.time_zone,e.status,e.transparency,"
        "e.provider_updated_at,e.raw_json,e.all_day_start_date,e.all_day_end_date,e.recurring_event_id,"
        "e.original_start_ms,e.original_start_date,e.recurrence_json,e.is_exception "
        "FROM events e WHERE e.calendar_id=? AND ");
    if (scope == QStringLiteral("series"))
        rowSql += QStringLiteral("(e.recurring_event_id=? OR e.provider_event_id=?) ");
    else if (scope == QStringLiteral("future") && !originalStartDate.isEmpty())
        rowSql += QStringLiteral("e.recurring_event_id=? AND e.original_start_date>=? ");
    else if (scope == QStringLiteral("future"))
        rowSql += QStringLiteral("e.recurring_event_id=? AND e.original_start_ms>=? ");
    else
        rowSql += QStringLiteral("e.provider_event_id=? ");
    rowSql += QStringLiteral("ORDER BY e.date_key");
    query.prepare(rowSql);
    query.addBindValue(calendarId);
    query.addBindValue(scope == QStringLiteral("occurrence") ? eventId : seriesId);
    if (scope == QStringLiteral("series")) query.addBindValue(seriesId);
    else if (scope == QStringLiteral("future"))
        query.addBindValue(originalStartDate.isEmpty() ? QVariant(originalStartMs) : QVariant(originalStartDate));
    if (!query.exec()) {
        setError(QStringLiteral("Event could not be deleted"), query.lastError().text());
        return {};
    }
    QJsonArray rows;
    QSet<QString> providerEventIds;
    while (query.next()) {
        providerEventIds.insert(query.value(0).toString());
        rows.append(QJsonObject {
            { QStringLiteral("providerEventId"), query.value(0).toString() },
            { QStringLiteral("etag"), query.value(1).toString() },
            { QStringLiteral("source"), query.value(2).toString() },
            { QStringLiteral("dateKey"), query.value(3).toString() },
            { QStringLiteral("startMs"), query.value(4).toDouble() },
            { QStringLiteral("endMs"), query.value(5).toDouble() },
            { QStringLiteral("allDay"), query.value(6).toBool() },
            { QStringLiteral("title"), query.value(7).toString() },
            { QStringLiteral("description"), query.value(8).toString() },
            { QStringLiteral("location"), query.value(9).toString() },
            { QStringLiteral("eventUrl"), query.value(10).toString() },
            { QStringLiteral("providerUid"), query.value(11).toString() },
            { QStringLiteral("timeZone"), query.value(12).toString() },
            { QStringLiteral("status"), query.value(13).toString() },
            { QStringLiteral("transparency"), query.value(14).toString() },
            { QStringLiteral("providerUpdatedAt"), query.value(15).toString() },
            { QStringLiteral("rawJson"), query.value(16).toString() },
            { QStringLiteral("allDayStartDate"), query.value(17).toString() },
            { QStringLiteral("allDayEndDate"), query.value(18).toString() },
            { QStringLiteral("seriesId"), query.value(19).toString() },
            { QStringLiteral("originalStartMs"), query.value(20).toDouble() },
            { QStringLiteral("originalStartDate"), query.value(21).toString() },
            { QStringLiteral("recurrence"), query.value(22).toString() },
            { QStringLiteral("isException"), query.value(23).toBool() }
        });
    }
    query.finish();
    if (rows.isEmpty()) {
        setError(QStringLiteral("Event could not be deleted"), QStringLiteral("event not found"));
        return {};
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QString mutationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject payload {
        { QStringLiteral("rows"), rows }, { QStringLiteral("scope"), scope },
        { QStringLiteral("seriesId"), seriesId },
        { QStringLiteral("scopeOriginalStartMs"), double(originalStartMs) },
        { QStringLiteral("scopeOriginalStartDate"), originalStartDate },
        { QStringLiteral("timeZone"), selectedTimeZone }
    };
    if (!m_database.transaction()) return {};
    QSqlQuery mutation(m_database);
    if (source == QStringLiteral("local-pending") && scope == QStringLiteral("occurrence")) {
        mutation.prepare(QStringLiteral(
            "SELECT id,payload_json,state FROM pending_mutations WHERE calendar_id=? AND provider_event_id=? "
            "AND operation='create' LIMIT 1"));
        mutation.addBindValue(calendarId);
        mutation.addBindValue(eventId);
        if (!mutation.exec() || !mutation.next()) {
            setError(QStringLiteral("Local event deletion could not be queued"), QStringLiteral("create mutation not found"));
            m_database.rollback();
            return {};
        }
        if (mutation.value(2).toString() == QStringLiteral("uploading")) {
            setError(QStringLiteral("Local event deletion is waiting for creation to finish"), {});
            m_database.rollback();
            return {};
        }
        mutationId = mutation.value(0).toString();
        payload.insert(QStringLiteral("createPayload"),
                       QJsonDocument::fromJson(mutation.value(1).toByteArray()).object());
        mutation.prepare(QStringLiteral(
            "UPDATE pending_mutations SET operation='cancel-create',payload_json=?,state='undoable',"
            "last_error='',updated_at=? WHERE id=?"));
        mutation.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        mutation.addBindValue(now);
        mutation.addBindValue(mutationId);
    } else {
        const QString operation = scope == QStringLiteral("series") ? QStringLiteral("delete-series")
            : scope == QStringLiteral("future") ? QStringLiteral("delete-future")
                                                  : QStringLiteral("delete");
        mutation.prepare(QStringLiteral(
            "INSERT INTO pending_mutations(id,account_id,calendar_id,provider_event_id,operation,payload_json,"
            "base_etag,state,created_at,updated_at) VALUES(?,?,?,?,?, ?,?,'undoable',?,?)"));
        mutation.addBindValue(mutationId);
        mutation.addBindValue(accountId);
        mutation.addBindValue(calendarId);
        mutation.addBindValue(scope == QStringLiteral("occurrence") ? eventId : seriesId);
        mutation.addBindValue(operation);
        mutation.addBindValue(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        mutation.addBindValue(scope == QStringLiteral("occurrence") ? etag : QStringLiteral(""));
        mutation.addBindValue(now);
        mutation.addBindValue(now);
    }
    QSqlQuery remove(m_database);
    bool removed = true;
    if (mutation.exec() && mutation.numRowsAffected() == 1) {
        for (const QString &providerEventId : providerEventIds) {
            remove.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND provider_event_id=?"));
            remove.addBindValue(calendarId);
            remove.addBindValue(providerEventId);
            if (!remove.exec() || remove.numRowsAffected() < 1) { removed = false; break; }
        }
    } else {
        removed = false;
    }
    if (!removed || !m_database.commit()) {
        setError(QStringLiteral("Event deletion could not be queued"),
                 mutation.lastError().text() + remove.lastError().text());
        m_database.rollback();
        return {};
    }
    m_lastError.clear();
    return mutationId;
}

bool Database::undoPendingDelete(const QString &mutationId)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT operation,payload_json,calendar_id,provider_event_id FROM pending_mutations "
        "WHERE id=? AND state='undoable' "
        "AND operation IN ('delete','delete-series','delete-future','cancel-create')"));
    query.addBindValue(mutationId);
    if (!query.exec() || !query.next()) {
        setError(QStringLiteral("Delete could not be undone"), QStringLiteral("undo window has closed"));
        return false;
    }
    const QString operation = query.value(0).toString();
    const QJsonObject payload = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
    const QString calendarId = query.value(2).toString();
    const QString eventId = query.value(3).toString();
    if (!m_database.transaction()) return false;
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO events(provider_event_id,date_key,calendar_id,start_ms,end_ms,all_day,title,description,"
        "location,event_url,provider_uid,time_zone,status,transparency,etag,provider_updated_at,raw_json,source,"
        "all_day_start_date,all_day_end_date,recurring_event_id,original_start_ms,original_start_date,"
        "recurrence_json,is_exception) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    for (const auto &value : payload.value(QStringLiteral("rows")).toArray()) {
        const QJsonObject row = value.toObject();
        int column = 0;
        insert.bindValue(column++, row.value(QStringLiteral("providerEventId")).toString(eventId));
        insert.bindValue(column++, row.value(QStringLiteral("dateKey")).toString());
        insert.bindValue(column++, calendarId);
        insert.bindValue(column++, qint64(row.value(QStringLiteral("startMs")).toDouble()));
        insert.bindValue(column++, qint64(row.value(QStringLiteral("endMs")).toDouble()));
        insert.bindValue(column++, row.value(QStringLiteral("allDay")).toBool() ? 1 : 0);
        insert.bindValue(column++, row.value(QStringLiteral("title")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("description")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("location")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("eventUrl")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("providerUid")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("timeZone")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("status")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("transparency")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("etag")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("providerUpdatedAt")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("rawJson")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("source")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("allDayStartDate")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("allDayEndDate")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("seriesId")).toString());
        insert.bindValue(column++, qint64(row.value(QStringLiteral("originalStartMs")).toDouble()));
        insert.bindValue(column++, row.value(QStringLiteral("originalStartDate")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("recurrence")).toString());
        insert.bindValue(column++, row.value(QStringLiteral("isException")).toBool() ? 1 : 0);
        if (!insert.exec()) {
            setError(QStringLiteral("Deleted event could not be restored"), insert.lastError().text());
            m_database.rollback();
            return false;
        }
    }
    QSqlQuery mutation(m_database);
    if (operation == QStringLiteral("cancel-create")) {
        mutation.prepare(QStringLiteral(
            "UPDATE pending_mutations SET operation='create',payload_json=?,state='queued',last_error='',updated_at=? WHERE id=?"));
        mutation.addBindValue(QString::fromUtf8(QJsonDocument(
            payload.value(QStringLiteral("createPayload")).toObject()).toJson(QJsonDocument::Compact)));
        mutation.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        mutation.addBindValue(mutationId);
    } else {
        mutation.prepare(QStringLiteral("DELETE FROM pending_mutations WHERE id=?"));
        mutation.addBindValue(mutationId);
    }
    if (!mutation.exec() || mutation.numRowsAffected() != 1 || !m_database.commit()) {
        setError(QStringLiteral("Delete undo could not be committed"), mutation.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::finalizePendingDelete(const QString &mutationId)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT operation FROM pending_mutations WHERE id=? AND state='undoable'"));
    query.addBindValue(mutationId);
    if (!query.exec() || !query.next()) return false;
    QSqlQuery finalize(m_database);
    if (query.value(0).toString() == QStringLiteral("cancel-create"))
        finalize.prepare(QStringLiteral("DELETE FROM pending_mutations WHERE id=?"));
    else
        finalize.prepare(QStringLiteral("UPDATE pending_mutations SET state='queued',updated_at=? WHERE id=?"));
    if (query.value(0).toString() != QStringLiteral("cancel-create"))
        finalize.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    finalize.addBindValue(mutationId);
    if (!finalize.exec() || finalize.numRowsAffected() != 1) {
        setError(QStringLiteral("Pending delete could not be finalized"), finalize.lastError().text());
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::finalizeUndoableDeletes()
{
    if (!m_database.transaction()) return false;
    QSqlQuery cancel(m_database);
    QSqlQuery queue(m_database);
    const bool ok = cancel.exec(QStringLiteral(
        "DELETE FROM pending_mutations WHERE state='undoable' AND operation='cancel-create'"))
        && queue.exec(QStringLiteral(
            "UPDATE pending_mutations SET state='queued',updated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now') "
            "WHERE state='undoable' AND operation IN ('delete','delete-series','delete-future')"));
    if (!ok || !m_database.commit()) {
        setError(QStringLiteral("Pending deletes could not be finalized"),
                 cancel.lastError().text() + queue.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

QJsonDocument Database::nextPendingMutation(const QString &accountId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT m.id,m.calendar_id,c.provider_calendar_id,m.provider_event_id,m.operation,"
        "m.payload_json,m.attempt_count,m.state,m.base_etag FROM pending_mutations m "
        "JOIN calendars c ON c.id=m.calendar_id "
        "WHERE m.account_id=? AND m.state IN ('queued','retrying','uploading') "
        "AND NOT EXISTS (SELECT 1 FROM pending_mutations earlier WHERE earlier.account_id=m.account_id "
        "AND earlier.rowid<m.rowid AND earlier.state!='undoable') "
        "ORDER BY m.created_at LIMIT 1"));
    query.addBindValue(accountId);
    if (!query.exec() || !query.next())
        return QJsonDocument(QJsonObject {});
    const QJsonDocument payload = QJsonDocument::fromJson(query.value(5).toByteArray());
    return QJsonDocument(QJsonObject {
        { QStringLiteral("id"), query.value(0).toString() },
        { QStringLiteral("calendarId"), query.value(1).toString() },
        { QStringLiteral("providerCalendarId"), query.value(2).toString() },
        { QStringLiteral("providerEventId"), query.value(3).toString() },
        { QStringLiteral("operation"), query.value(4).toString() },
        { QStringLiteral("payload"), payload.object() },
        { QStringLiteral("attemptCount"), query.value(6).toInt() },
        { QStringLiteral("state"), query.value(7).toString() },
        { QStringLiteral("baseEtag"), query.value(8).toString() }
    });
}

bool Database::setMutationState(const QString &mutationId, const QString &state,
                                const QString &error, bool incrementAttempt)
{
    QSqlQuery query(m_database);
    query.prepare(incrementAttempt
        ? QStringLiteral("UPDATE pending_mutations SET state=?,last_error=?,attempt_count=attempt_count+1,updated_at=? WHERE id=?")
        : QStringLiteral("UPDATE pending_mutations SET state=?,last_error=?,updated_at=? WHERE id=?"));
    query.addBindValue(state);
    query.addBindValue(error.isNull() ? QStringLiteral("") : error);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(mutationId);
    if (query.exec() && query.numRowsAffected() == 1)
        return true;
    setError(QStringLiteral("Mutation state could not be updated"), query.lastError().text());
    return false;
}

QJsonDocument Database::pendingMutations() const
{
    QJsonArray result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT m.id,m.operation,m.state,m.attempt_count,m.last_error,m.created_at,m.updated_at,"
        "m.provider_event_id,m.payload_json,c.name,a.email,"
        "(SELECT COUNT(*) FROM pending_mutations active WHERE active.state='uploading') AS uploading_count "
        "FROM pending_mutations m JOIN calendars c ON c.id=m.calendar_id "
        "JOIN accounts a ON a.id=m.account_id WHERE m.state!='undoable' "
        "ORDER BY m.created_at"));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return QJsonDocument(result);
    }
    while (query.next()) {
        const QJsonObject payload = QJsonDocument::fromJson(query.value(8).toByteArray()).object();
        QString title = payload.value(QStringLiteral("title")).toString();
        if (title.isEmpty()) {
            const QJsonArray rows = payload.value(QStringLiteral("rows")).toArray();
            if (!rows.isEmpty()) title = rows.first().toObject().value(QStringLiteral("title")).toString();
        }
        if (title.isEmpty()) title = QStringLiteral("Untitled event");
        const QString state = query.value(2).toString();
        result.append(QJsonObject {
            { QStringLiteral("id"), query.value(0).toString() },
            { QStringLiteral("operation"), query.value(1).toString() },
            { QStringLiteral("state"), state },
            { QStringLiteral("attemptCount"), query.value(3).toInt() },
            { QStringLiteral("lastError"), query.value(4).toString() },
            { QStringLiteral("createdAt"), query.value(5).toString() },
            { QStringLiteral("updatedAt"), query.value(6).toString() },
            { QStringLiteral("eventId"), query.value(7).toString() },
            { QStringLiteral("title"), title },
            { QStringLiteral("calendarName"), query.value(9).toString() },
            { QStringLiteral("accountEmail"), query.value(10).toString() },
            { QStringLiteral("canRetry"), state == QStringLiteral("failed")
                || state == QStringLiteral("conflict") || state == QStringLiteral("blocked")
                || state == QStringLiteral("retrying") },
            { QStringLiteral("canDiscard"), state != QStringLiteral("uploading")
                && query.value(11).toInt() == 0 }
        });
    }
    return QJsonDocument(result);
}

QJsonDocument Database::takeNewInvitations()
{
    QJsonArray result;
    if (!m_database.transaction()) return QJsonDocument(result);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT e.calendar_id,e.provider_event_id,e.title,e.start_ms,e.all_day,c.name "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE e.rowid IN (SELECT MIN(rowid) FROM events GROUP BY calendar_id,provider_event_id) "
        "AND e.end_ms>=? AND c.selected=1 AND json_valid(e.raw_json) "
        "AND COALESCE(json_extract(e.raw_json,'$.organizer.self'),0)=0 "
        "AND EXISTS (SELECT 1 FROM json_each(e.raw_json,'$.attendees') WHERE json_extract(value,'$.self')=1 "
        "AND json_extract(value,'$.responseStatus')='needsAction') "
        "AND NOT EXISTS (SELECT 1 FROM invitation_notifications n WHERE n.calendar_id=e.calendar_id "
        "AND n.event_id=e.provider_event_id) ORDER BY e.start_ms"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        setError(QStringLiteral("New invitations could not be read"), query.lastError().text());
        m_database.rollback();
        return QJsonDocument(result);
    }
    QSqlQuery mark(m_database);
    mark.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO invitation_notifications(calendar_id,event_id,response_status,notified_at) "
        "VALUES(?,?,'needsAction',?)"));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    while (query.next()) {
        result.append(QJsonObject {
            { QStringLiteral("calendarId"), query.value(0).toString() },
            { QStringLiteral("eventId"), query.value(1).toString() },
            { QStringLiteral("title"), query.value(2).toString() },
            { QStringLiteral("startMs"), query.value(3).toDouble() },
            { QStringLiteral("allDay"), query.value(4).toBool() },
            { QStringLiteral("calendarName"), query.value(5).toString() }
        });
        mark.bindValue(0, query.value(0));
        mark.bindValue(1, query.value(1));
        mark.bindValue(2, now);
        if (!mark.exec()) {
            setError(QStringLiteral("Invitation notification could not be recorded"), mark.lastError().text());
            m_database.rollback();
            return QJsonDocument(QJsonArray {});
        }
    }
    if (!m_database.commit()) return QJsonDocument(QJsonArray {});
    m_lastError.clear();
    return QJsonDocument(result);
}

QJsonDocument Database::dueReminders(qint64 nowMs) const
{
    QJsonArray result;
    QSqlQuery initialized(QStringLiteral(
        "SELECT value FROM metadata WHERE key='reminder_scheduler_initialized_ms'"), m_database);
    const qint64 initializedMs = initialized.next() ? initialized.value(0).toLongLong() : nowMs;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT e.calendar_id,e.provider_event_id,e.title,e.start_ms,e.end_ms,e.all_day,e.event_url,"
        "COALESCE((SELECT p.payload_json FROM pending_mutations p WHERE p.calendar_id=e.calendar_id "
        "AND p.provider_event_id=e.provider_event_id AND p.operation IN ('update','update-series','update-future') "
        "ORDER BY p.created_at DESC LIMIT 1),e.raw_json),c.default_reminders,c.name "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE e.rowid IN (SELECT MIN(rowid) FROM events GROUP BY calendar_id,provider_event_id,start_ms) "
        "AND e.end_ms>? AND e.start_ms<=? AND c.selected=1 AND e.status!='cancelled' ORDER BY e.start_ms"));
    query.addBindValue(nowMs - 5 * 60 * 1000);
    query.addBindValue(nowMs + qint64(28) * 24 * 60 * 60 * 1000);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return QJsonDocument(result);
    }
    QSqlQuery delivery(m_database);
    delivery.prepare(QStringLiteral(
        "SELECT state,scheduled_ms FROM reminder_deliveries WHERE id=?"));
    while (query.next()) {
        const QJsonObject raw = QJsonDocument::fromJson(query.value(7).toByteArray()).object();
        QJsonObject reminders = raw.value(QStringLiteral("reminders")).toObject();
        QJsonArray overrides = reminders.value(QStringLiteral("overrides")).toArray();
        if (reminders.isEmpty() || reminders.value(QStringLiteral("useDefault")).toBool())
            overrides = QJsonDocument::fromJson(query.value(8).toByteArray()).array();
        for (const QJsonValue &value : overrides) {
            const QJsonObject reminder = value.toObject();
            if (reminder.value(QStringLiteral("method")).toString(QStringLiteral("popup"))
                != QStringLiteral("popup")) continue;
            const int minutes = reminder.value(QStringLiteral("minutes")).toInt(-1);
            if (minutes < 0) continue;
            const qint64 originalScheduled = query.value(3).toLongLong() - qint64(minutes) * 60000;
            const QByteArray identity = query.value(0).toByteArray() + '\0' + query.value(1).toByteArray()
                + '\0' + QByteArray::number(query.value(3).toLongLong()) + '\0' + QByteArray::number(minutes);
            const QString id = QString::fromLatin1(
                QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
            delivery.bindValue(0, id);
            QString state;
            qint64 scheduled = originalScheduled;
            if (delivery.exec() && delivery.next()) {
                state = delivery.value(0).toString();
                scheduled = delivery.value(1).toLongLong();
            }
            if (state == QStringLiteral("delivered") || state == QStringLiteral("dismissed")
                || scheduled > nowMs || (state.isEmpty() && originalScheduled < initializedMs)) continue;
            const QJsonArray links = meetingLinks(raw);
            result.append(QJsonObject {
                { QStringLiteral("id"), id },
                { QStringLiteral("calendarId"), query.value(0).toString() },
                { QStringLiteral("eventId"), query.value(1).toString() },
                { QStringLiteral("title"), query.value(2).toString() },
                { QStringLiteral("startMs"), query.value(3).toDouble() },
                { QStringLiteral("endMs"), query.value(4).toDouble() },
                { QStringLiteral("allDay"), query.value(5).toBool() },
                { QStringLiteral("eventUrl"), query.value(6).toString() },
                { QStringLiteral("calendarName"), query.value(9).toString() },
                { QStringLiteral("minutes"), minutes },
                { QStringLiteral("scheduledMs"), double(scheduled) },
                { QStringLiteral("meetingLinks"), links }
            });
        }
    }
    return QJsonDocument(result);
}

bool Database::markReminderDelivered(const QString &reminderId, uint notificationId)
{
    const QJsonArray due = dueReminders(QDateTime::currentMSecsSinceEpoch()).array();
    QJsonObject target;
    for (const QJsonValue &value : due)
        if (value.toObject().value(QStringLiteral("id")).toString() == reminderId) target = value.toObject();
    if (target.isEmpty()) return false;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO reminder_deliveries(id,calendar_id,event_id,start_ms,minutes,scheduled_ms,state,notification_id,updated_at) "
        "VALUES(?,?,?,?,?,?,'delivered',?,?) ON CONFLICT(id) DO UPDATE SET state='delivered',"
        "notification_id=excluded.notification_id,updated_at=excluded.updated_at"));
    query.addBindValue(reminderId);
    query.addBindValue(target.value(QStringLiteral("calendarId")).toString());
    query.addBindValue(target.value(QStringLiteral("eventId")).toString());
    query.addBindValue(qint64(target.value(QStringLiteral("startMs")).toDouble()));
    query.addBindValue(target.value(QStringLiteral("minutes")).toInt());
    query.addBindValue(qint64(target.value(QStringLiteral("scheduledMs")).toDouble()));
    query.addBindValue(notificationId);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    return query.exec();
}

bool Database::snoozeReminder(const QString &reminderId, qint64 untilMs)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE reminder_deliveries SET state='snoozed',scheduled_ms=?,notification_id=0,updated_at=? WHERE id=?"));
    query.addBindValue(untilMs);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(reminderId);
    return query.exec() && query.numRowsAffected() == 1;
}

bool Database::dismissReminder(const QString &reminderId)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE reminder_deliveries SET state='dismissed',notification_id=0,updated_at=? WHERE id=?"));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(reminderId);
    return query.exec() && query.numRowsAffected() == 1;
}

QString Database::reminderIdForNotification(uint notificationId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id FROM reminder_deliveries WHERE notification_id=? LIMIT 1"));
    query.addBindValue(notificationId);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool Database::pruneReminderDeliveries(qint64 beforeStartMs)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM reminder_deliveries WHERE start_ms<?"));
    query.addBindValue(beforeStartMs);
    return query.exec();
}

bool Database::retryMutation(const QString &mutationId)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "UPDATE pending_mutations SET state='queued',last_error='',updated_at=? WHERE id=? "
        "AND state IN ('failed','conflict','blocked','retrying')"));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(mutationId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        setError(QStringLiteral("Queued change could not be retried"),
                 query.lastError().text().isEmpty() ? QStringLiteral("change is no longer retryable")
                                                    : query.lastError().text());
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::discardMutation(const QString &mutationId)
{
    QSqlQuery selected(m_database);
    selected.prepare(QStringLiteral(
        "SELECT operation,state,calendar_id,provider_event_id,payload_json FROM pending_mutations WHERE id=?"));
    selected.addBindValue(mutationId);
    if (!selected.exec() || !selected.next()) {
        setError(QStringLiteral("Queued change could not be discarded"), QStringLiteral("change was not found"));
        return false;
    }
    const QString operation = selected.value(0).toString();
    const QString state = selected.value(1).toString();
    const QString calendarId = selected.value(2).toString();
    const QString eventId = selected.value(3).toString();
    const QJsonObject payload = QJsonDocument::fromJson(selected.value(4).toByteArray()).object();
    if (state == QStringLiteral("uploading") || state == QStringLiteral("undoable")) {
        setError(QStringLiteral("Queued change could not be discarded"),
                 QStringLiteral("change is currently being processed"));
        return false;
    }
    QSqlQuery active(m_database);
    if (!active.exec(QStringLiteral("SELECT 1 FROM pending_mutations WHERE state='uploading' LIMIT 1"))
        || active.next()) {
        setError(QStringLiteral("Queued change could not be discarded"),
                 QStringLiteral("another change is currently being uploaded"));
        return false;
    }
    if (operation.startsWith(QStringLiteral("delete"))) {
        QSqlQuery makeUndoable(m_database);
        makeUndoable.prepare(QStringLiteral("UPDATE pending_mutations SET state='undoable' WHERE id=?"));
        makeUndoable.addBindValue(mutationId);
        if (!makeUndoable.exec() || makeUndoable.numRowsAffected() != 1)
            return false;
        return undoPendingDelete(mutationId);
    }

    if (!m_database.transaction()) return false;
    QSqlQuery removeEvents(m_database);
    if (operation == QStringLiteral("move")) {
        removeEvents.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND provider_event_id=?"));
        removeEvents.addBindValue(payload.value(QStringLiteral("targetCalendarId")).toString());
    } else {
        removeEvents.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND provider_event_id=?"));
        removeEvents.addBindValue(calendarId);
    }
    removeEvents.addBindValue(eventId);
    QSqlQuery removeMutation(m_database);
    removeMutation.prepare(QStringLiteral("DELETE FROM pending_mutations WHERE id=?"));
    removeMutation.addBindValue(mutationId);
    if (!removeEvents.exec() || !removeMutation.exec() || removeMutation.numRowsAffected() != 1
        || !m_database.commit()) {
        setError(QStringLiteral("Queued change could not be discarded"),
                 removeEvents.lastError().text() + removeMutation.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::completeCreateMutation(const QString &mutationId, const QJsonObject &remoteEvent)
{
    const QString remoteId = remoteEvent.value(QStringLiteral("id")).toString();
    if (remoteId.isEmpty()) {
        setError(QStringLiteral("Google create response was incomplete"), QStringLiteral("missing event id"));
        return false;
    }
    if (!m_database.transaction()) {
        setError(QStringLiteral("Mutation completion transaction could not start"), m_database.lastError().text());
        return false;
    }
    QSqlQuery mutation(m_database);
    mutation.prepare(QStringLiteral("SELECT calendar_id,provider_event_id FROM pending_mutations WHERE id=? AND operation='create'"));
    mutation.addBindValue(mutationId);
    if (!mutation.exec() || !mutation.next()) {
        setError(QStringLiteral("Queued create could not be found"), mutation.lastError().text());
        m_database.rollback();
        return false;
    }
    const QString calendarId = mutation.value(0).toString();
    const QString localId = mutation.value(1).toString();
    const QJsonArray recurrence = remoteEvent.value(QStringLiteral("recurrence")).toArray();
    const QString recurrenceJson = recurrence.isEmpty() ? QStringLiteral("")
        : QString::fromUtf8(QJsonDocument(recurrence).toJson(QJsonDocument::Compact));
    const QString recurringEventId = recurrence.isEmpty()
        ? remoteEvent.value(QStringLiteral("recurringEventId")).toString(QStringLiteral(""))
        : remoteId;
    const QJsonObject originalStartValue = remoteEvent.value(QStringLiteral("originalStartTime")).toObject();
    const QJsonObject startValue = remoteEvent.value(QStringLiteral("start")).toObject();
    const QString originalStartDate = originalStartValue.value(QStringLiteral("date")).toString(QStringLiteral(""));
    const QDateTime originalStart = googleDateTime(
        originalStartValue, startValue.value(QStringLiteral("timeZone")).toString());
    const qint64 originalStartMs = originalStartValue.contains(QStringLiteral("dateTime"))
        && originalStart.isValid() ? originalStart.toMSecsSinceEpoch() : 0;
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE events SET provider_event_id=?,provider_uid=?,event_url=?,etag=?,"
        "provider_updated_at=?,raw_json=?,source='google',recurring_event_id=?,"
        "original_start_ms=?,original_start_date=?,recurrence_json=?,is_exception=0 "
        "WHERE calendar_id=? AND provider_event_id=? AND source='local-pending'"));
    update.addBindValue(remoteId);
    update.addBindValue(remoteEvent.value(QStringLiteral("iCalUID")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("htmlLink")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("etag")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("updated")).toString(QStringLiteral("")));
    update.addBindValue(QString::fromUtf8(QJsonDocument(remoteEvent).toJson(QJsonDocument::Compact)));
    update.addBindValue(recurringEventId);
    update.addBindValue(originalStartMs);
    update.addBindValue(originalStartDate);
    update.addBindValue(recurrenceJson);
    update.addBindValue(calendarId);
    update.addBindValue(localId);
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM pending_mutations WHERE id=?"));
    remove.addBindValue(mutationId);
    if (!update.exec() || update.numRowsAffected() < 1 || !remove.exec() || !m_database.commit()) {
        setError(QStringLiteral("Google event could not replace its local draft"),
                 update.lastError().text() + remove.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::rebaseUpdateMutation(const QString &mutationId, const QJsonObject &remoteEvent)
{
    const QString remoteEtag = remoteEvent.value(QStringLiteral("etag")).toString();
    if (remoteEtag.isEmpty()) {
        setError(QStringLiteral("Event update could not be rebased"),
                 QStringLiteral("Google returned no ETag"));
        return false;
    }

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT m.payload_json,e.raw_json,m.calendar_id,m.provider_event_id "
        "FROM pending_mutations m JOIN events e ON e.calendar_id=m.calendar_id "
        "AND e.provider_event_id=m.provider_event_id "
        "WHERE m.id=? AND m.operation='update' LIMIT 1"));
    query.addBindValue(mutationId);
    if (!query.exec() || !query.next()) {
        setError(QStringLiteral("Event update could not be rebased"), QStringLiteral("queued event not found"));
        return false;
    }

    QJsonObject local = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
    const QJsonObject base = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
    const QString calendarId = query.value(2).toString();
    const QString providerEventId = query.value(3).toString();
    QStringList conflicts;

    const auto mergeText = [&](const QString &localKey, const QString &remoteKey,
                               const QString &label) {
        const QString baseValue = base.value(remoteKey).toString();
        const QString localValue = local.value(localKey).toString();
        const QString remoteValue = remoteEvent.value(remoteKey).toString();
        const bool localChanged = localValue != baseValue;
        const bool remoteChanged = remoteValue != baseValue;
        if (localChanged && remoteChanged && localValue != remoteValue)
            conflicts.append(label);
        else if (!localChanged)
            local.insert(localKey, remoteValue);
    };
    mergeText(QStringLiteral("title"), QStringLiteral("summary"), QStringLiteral("title"));
    mergeText(QStringLiteral("description"), QStringLiteral("description"), QStringLiteral("notes"));
    mergeText(QStringLiteral("location"), QStringLiteral("location"), QStringLiteral("location"));
    const auto mergeJson = [&](const QString &key, const QString &label) {
        const QJsonValue baseValue = base.value(key);
        const QJsonValue localValue = local.value(key);
        const QJsonValue remoteValue = remoteEvent.value(key);
        const bool localChanged = localValue != baseValue;
        const bool remoteChanged = remoteValue != baseValue;
        if (localChanged && remoteChanged && localValue != remoteValue)
            conflicts.append(label);
        else if (!localChanged)
            local.insert(key, remoteValue);
    };
    mergeJson(QStringLiteral("attendees"), QStringLiteral("guests"));
    mergeJson(QStringLiteral("reminders"), QStringLiteral("reminders"));
    mergeJson(QStringLiteral("visibility"), QStringLiteral("visibility"));
    mergeJson(QStringLiteral("transparency"), QStringLiteral("availability"));
    mergeJson(QStringLiteral("guestsCanInviteOthers"), QStringLiteral("guest invitation permission"));
    mergeJson(QStringLiteral("guestsCanModify"), QStringLiteral("guest editing permission"));
    mergeJson(QStringLiteral("guestsCanSeeOtherGuests"), QStringLiteral("guest-list permission"));

    const QJsonObject baseStart = base.value(QStringLiteral("start")).toObject();
    const QJsonObject baseEnd = base.value(QStringLiteral("end")).toObject();
    const QJsonObject remoteStart = remoteEvent.value(QStringLiteral("start")).toObject();
    const QJsonObject remoteEnd = remoteEvent.value(QStringLiteral("end")).toObject();
    const QString baseZone = baseStart.value(QStringLiteral("timeZone")).toString();
    const QString remoteZone = remoteStart.value(QStringLiteral("timeZone")).toString(baseZone);
    const qint64 baseStartMs = googleDateTime(baseStart, baseZone).toMSecsSinceEpoch();
    const qint64 baseEndMs = googleDateTime(baseEnd, baseZone).toMSecsSinceEpoch();
    const qint64 remoteStartMs = googleDateTime(remoteStart, remoteZone).toMSecsSinceEpoch();
    const qint64 remoteEndMs = googleDateTime(remoteEnd, remoteZone).toMSecsSinceEpoch();
    const qint64 localStartMs = qint64(local.value(QStringLiteral("startMs")).toDouble());
    const qint64 localEndMs = qint64(local.value(QStringLiteral("endMs")).toDouble());
    const bool baseAllDay = baseStart.contains(QStringLiteral("date"));
    const bool remoteAllDay = remoteStart.contains(QStringLiteral("date"));
    const bool localAllDay = local.value(QStringLiteral("allDay")).toBool();

    const auto mergeNumber = [&](const QString &key, qint64 localValue, qint64 baseValue,
                                 qint64 remoteValue, const QString &label) {
        const bool localChanged = localValue != baseValue;
        const bool remoteChanged = remoteValue != baseValue;
        if (localChanged && remoteChanged && localValue != remoteValue)
            conflicts.append(label);
        else if (!localChanged)
            local.insert(key, double(remoteValue));
    };
    if (localAllDay != baseAllDay && remoteAllDay != baseAllDay && localAllDay != remoteAllDay)
        conflicts.append(QStringLiteral("all-day setting"));
    else if (localAllDay == baseAllDay)
        local.insert(QStringLiteral("allDay"), remoteAllDay);
    if (local.value(QStringLiteral("timeZone")).toString(baseZone) == baseZone)
        local.insert(QStringLiteral("timeZone"), remoteZone);

    const bool mergedAllDay = local.value(QStringLiteral("allDay")).toBool();
    if (mergedAllDay && baseAllDay && remoteAllDay) {
        const auto mergeDate = [&](const QString &localKey, const QJsonObject &baseValue,
                                   const QJsonObject &remoteValue, const QString &label) {
            const QString baseDate = baseValue.value(QStringLiteral("date")).toString();
            const QString localDate = local.value(localKey).toString(baseDate);
            const QString remoteDate = remoteValue.value(QStringLiteral("date")).toString();
            const bool localChanged = localDate != baseDate;
            const bool remoteChanged = remoteDate != baseDate;
            if (localChanged && remoteChanged && localDate != remoteDate)
                conflicts.append(label);
            else if (!localChanged)
                local.insert(localKey, remoteDate);
        };
        mergeDate(QStringLiteral("allDayStartDate"), baseStart, remoteStart,
                  QStringLiteral("start date"));
        mergeDate(QStringLiteral("allDayEndDate"), baseEnd, remoteEnd,
                  QStringLiteral("end date"));
    } else if (mergedAllDay && localAllDay == baseAllDay && remoteAllDay) {
        local.insert(QStringLiteral("allDayStartDate"), remoteStart.value(QStringLiteral("date")));
        local.insert(QStringLiteral("allDayEndDate"), remoteEnd.value(QStringLiteral("date")));
    } else if (!mergedAllDay) {
        mergeNumber(QStringLiteral("startMs"), localStartMs, baseStartMs, remoteStartMs,
                    QStringLiteral("start time"));
        mergeNumber(QStringLiteral("endMs"), localEndMs, baseEndMs, remoteEndMs,
                    QStringLiteral("end time"));
    }

    if (!conflicts.isEmpty()) {
        setError(QStringLiteral("Event update conflicts with changes from Google"), conflicts.join(QStringLiteral(", ")));
        return false;
    }

    QTimeZone zone(local.value(QStringLiteral("timeZone")).toString().toUtf8());
    if (!zone.isValid()) zone = QTimeZone::systemTimeZone();
    QString allDayStart = QStringLiteral("");
    QString allDayEnd = QStringLiteral("");
    QDate firstDate;
    QDate lastDate;
    qint64 mergedStartMs = qint64(local.value(QStringLiteral("startMs")).toDouble());
    qint64 mergedEndMs = qint64(local.value(QStringLiteral("endMs")).toDouble());
    if (mergedAllDay) {
        allDayStart = local.value(QStringLiteral("allDayStartDate")).toString();
        allDayEnd = local.value(QStringLiteral("allDayEndDate")).toString();
        firstDate = QDate::fromString(allDayStart, Qt::ISODate);
        lastDate = QDate::fromString(allDayEnd, Qt::ISODate).addDays(-1);
        mergedStartMs = QDateTime(firstDate, QTime(0, 0), zone).toMSecsSinceEpoch();
        mergedEndMs = QDateTime(lastDate.addDays(1), QTime(0, 0), zone).toMSecsSinceEpoch();
        local.insert(QStringLiteral("startMs"), double(mergedStartMs));
        local.insert(QStringLiteral("endMs"), double(mergedEndMs));
    } else {
        firstDate = QDateTime::fromMSecsSinceEpoch(mergedStartMs).date();
        lastDate = QDateTime::fromMSecsSinceEpoch(mergedEndMs - 1).date();
        local.remove(QStringLiteral("allDayStartDate"));
        local.remove(QStringLiteral("allDayEndDate"));
    }
    if (mergedStartMs <= 0 || mergedEndMs <= mergedStartMs
        || !firstDate.isValid() || !lastDate.isValid() || lastDate < firstDate) {
        setError(QStringLiteral("Event update could not be rebased"), QStringLiteral("Google returned invalid times"));
        return false;
    }

    if (!m_database.transaction()) return false;
    QSqlQuery mutation(m_database);
    mutation.prepare(QStringLiteral(
        "UPDATE pending_mutations SET payload_json=?,base_etag=?,state='queued',last_error='',updated_at=? WHERE id=?"));
    mutation.addBindValue(QString::fromUtf8(QJsonDocument(local).toJson(QJsonDocument::Compact)));
    mutation.addBindValue(remoteEtag);
    mutation.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    mutation.addBindValue(mutationId);
    QSqlQuery removeRows(m_database);
    removeRows.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND provider_event_id=?"));
    removeRows.addBindValue(calendarId);
    removeRows.addBindValue(providerEventId);
    QSqlQuery event(m_database);
    event.prepare(QStringLiteral(
        "INSERT INTO events(provider_event_id,date_key,calendar_id,start_ms,end_ms,all_day,title,description,"
        "location,event_url,provider_uid,time_zone,status,transparency,etag,provider_updated_at,raw_json,source,"
        "all_day_start_date,all_day_end_date,recurring_event_id,original_start_ms,original_start_date,"
        "recurrence_json,is_exception) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,'google',?,?,?,?,?,?,?)"));
    const QString remoteJson = QString::fromUtf8(QJsonDocument(remoteEvent).toJson(QJsonDocument::Compact));
    const QString recurringEventId = remoteEvent.value(QStringLiteral("recurringEventId")).toString(QStringLiteral(""));
    const QJsonObject originalStartValue = remoteEvent.value(QStringLiteral("originalStartTime")).toObject();
    const QString originalStartDate = originalStartValue.value(QStringLiteral("date")).toString(QStringLiteral(""));
    const QDateTime originalStart = googleDateTime(originalStartValue, QString::fromUtf8(zone.id()));
    const qint64 originalStartMs = originalStartValue.contains(QStringLiteral("dateTime")) && originalStart.isValid()
        ? originalStart.toMSecsSinceEpoch() : 0;
    const QString recurrenceJson = remoteEvent.contains(QStringLiteral("recurrence"))
        ? QString::fromUtf8(QJsonDocument(remoteEvent.value(QStringLiteral("recurrence")).toArray()).toJson(QJsonDocument::Compact))
        : QStringLiteral("");
    const bool isException = !recurringEventId.isEmpty()
        && ((!originalStartDate.isEmpty() && originalStartDate != allDayStart)
            || (originalStartMs > 0 && originalStartMs != mergedStartMs));
    bool rowsStored = true;
    if (!removeRows.exec()) rowsStored = false;
    for (QDate date = firstDate; rowsStored && date <= lastDate; date = date.addDays(1)) {
        int column = 0;
        event.bindValue(column++, providerEventId);
        event.bindValue(column++, date.toString(Qt::ISODate));
        event.bindValue(column++, calendarId);
        event.bindValue(column++, mergedStartMs);
        event.bindValue(column++, mergedEndMs);
        event.bindValue(column++, mergedAllDay ? 1 : 0);
        event.bindValue(column++, local.value(QStringLiteral("title")).toString(QStringLiteral("")));
        event.bindValue(column++, local.value(QStringLiteral("description")).toString(QStringLiteral("")));
        event.bindValue(column++, local.value(QStringLiteral("location")).toString(QStringLiteral("")));
        event.bindValue(column++, remoteEvent.value(QStringLiteral("htmlLink")).toString(QStringLiteral("")));
        event.bindValue(column++, remoteEvent.value(QStringLiteral("iCalUID")).toString(QStringLiteral("")));
        event.bindValue(column++, QString::fromUtf8(zone.id()));
        event.bindValue(column++, remoteEvent.value(QStringLiteral("status")).toString(QStringLiteral("confirmed")));
        event.bindValue(column++, remoteEvent.value(QStringLiteral("transparency")).toString(QStringLiteral("opaque")));
        event.bindValue(column++, remoteEtag);
        event.bindValue(column++, remoteEvent.value(QStringLiteral("updated")).toString(QStringLiteral("")));
        event.bindValue(column++, remoteJson);
        event.bindValue(column++, allDayStart);
        event.bindValue(column++, allDayEnd);
        event.bindValue(column++, recurringEventId);
        event.bindValue(column++, originalStartMs);
        event.bindValue(column++, originalStartDate);
        event.bindValue(column++, recurrenceJson);
        event.bindValue(column++, isException ? 1 : 0);
        if (!event.exec()) rowsStored = false;
    }
    if (!mutation.exec() || mutation.numRowsAffected() != 1
        || !rowsStored || !m_database.commit()) {
        setError(QStringLiteral("Event update rebase could not be stored"),
                 mutation.lastError().text() + removeRows.lastError().text() + event.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::rebaseRsvpMutation(const QString &mutationId, const QJsonObject &remoteEvent)
{
    const QString remoteEtag = remoteEvent.value(QStringLiteral("etag")).toString();
    if (remoteEtag.isEmpty()) {
        setError(QStringLiteral("Invitation response could not be rebased"), QStringLiteral("Google returned no ETag"));
        return false;
    }
    QSqlQuery selected(m_database);
    selected.prepare(QStringLiteral(
        "SELECT payload_json,calendar_id,provider_event_id FROM pending_mutations "
        "WHERE id=? AND operation='rsvp'"));
    selected.addBindValue(mutationId);
    if (!selected.exec() || !selected.next()) {
        setError(QStringLiteral("Invitation response could not be rebased"), QStringLiteral("queued response not found"));
        return false;
    }
    const QJsonObject local = QJsonDocument::fromJson(selected.value(0).toByteArray()).object();
    const QString response = local.value(QStringLiteral("responseStatus")).toString();
    QJsonObject rebased = remoteEvent;
    QJsonArray attendees = rebased.value(QStringLiteral("attendees")).toArray();
    bool foundSelf = false;
    for (qsizetype index = 0; index < attendees.size(); ++index) {
        QJsonObject attendee = attendees.at(index).toObject();
        if (!attendee.value(QStringLiteral("self")).toBool()) continue;
        attendee.insert(QStringLiteral("responseStatus"), response);
        attendees.replace(index, attendee);
        foundSelf = true;
        break;
    }
    if (!foundSelf) {
        setError(QStringLiteral("Invitation response could not be rebased"),
                 QStringLiteral("Google no longer lists the signed-in user as an attendee"));
        return false;
    }
    rebased.insert(QStringLiteral("attendees"), attendees);
    rebased.insert(QStringLiteral("responseStatus"), response);
    rebased.insert(QStringLiteral("title"), rebased.value(QStringLiteral("summary")));
    if (!m_database.transaction()) return false;
    QSqlQuery mutation(m_database);
    mutation.prepare(QStringLiteral(
        "UPDATE pending_mutations SET payload_json=?,base_etag=?,state='queued',last_error='',updated_at=? WHERE id=?"));
    mutation.addBindValue(QString::fromUtf8(QJsonDocument(rebased).toJson(QJsonDocument::Compact)));
    mutation.addBindValue(remoteEtag);
    mutation.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    mutation.addBindValue(mutationId);
    QSqlQuery event(m_database);
    event.prepare(QStringLiteral(
        "UPDATE events SET etag=?,provider_updated_at=?,raw_json=? WHERE calendar_id=? AND provider_event_id=?"));
    event.addBindValue(remoteEtag);
    event.addBindValue(remoteEvent.value(QStringLiteral("updated")).toString());
    event.addBindValue(QString::fromUtf8(QJsonDocument(remoteEvent).toJson(QJsonDocument::Compact)));
    event.addBindValue(selected.value(1).toString());
    event.addBindValue(selected.value(2).toString());
    if (!mutation.exec() || mutation.numRowsAffected() != 1 || !event.exec()
        || event.numRowsAffected() < 1 || !m_database.commit()) {
        setError(QStringLiteral("Invitation response rebase could not be stored"),
                 mutation.lastError().text() + event.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::completeUpdateMutation(const QString &mutationId, const QJsonObject &remoteEvent)
{
    const QString remoteId = remoteEvent.value(QStringLiteral("id")).toString();
    if (remoteId.isEmpty()) {
        setError(QStringLiteral("Google update response was incomplete"), QStringLiteral("missing event id"));
        return false;
    }
    if (!m_database.transaction()) return false;
    QSqlQuery mutation(m_database);
    mutation.prepare(QStringLiteral(
        "SELECT calendar_id,provider_event_id,operation FROM pending_mutations "
        "WHERE id=? AND operation IN ('update','update-series','update-future','rsvp')"));
    mutation.addBindValue(mutationId);
    if (!mutation.exec() || !mutation.next()) {
        setError(QStringLiteral("Queued update could not be found"), mutation.lastError().text());
        m_database.rollback();
        return false;
    }
    const QString calendarId = mutation.value(0).toString();
    const QString providerEventId = mutation.value(1).toString();
    const QString operation = mutation.value(2).toString();
    const bool seriesUpdate = operation == QStringLiteral("update-series")
        || operation == QStringLiteral("update-future");
    QSqlQuery pendingDelete(m_database);
    pendingDelete.prepare(QStringLiteral(
        "SELECT 1 FROM pending_mutations WHERE calendar_id=? AND provider_event_id=? "
        "AND operation='delete' LIMIT 1"));
    pendingDelete.addBindValue(calendarId);
    pendingDelete.addBindValue(providerEventId);
    const bool deleting = pendingDelete.exec() && pendingDelete.next();
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE events SET event_url=?,provider_uid=?,etag=?,provider_updated_at=?,raw_json=?,source='google' "
        "WHERE calendar_id=? AND provider_event_id=?"));
    update.addBindValue(remoteEvent.value(QStringLiteral("htmlLink")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("iCalUID")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("etag")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("updated")).toString(QStringLiteral("")));
    update.addBindValue(QString::fromUtf8(QJsonDocument(remoteEvent).toJson(QJsonDocument::Compact)));
    update.addBindValue(calendarId);
    update.addBindValue(providerEventId);
    QSqlQuery advance(m_database);
    advance.prepare(QStringLiteral(
        "UPDATE pending_mutations SET base_etag=?,updated_at=? WHERE calendar_id=? "
        "AND provider_event_id=? AND operation IN ('update','delete','rsvp') AND id!=?"));
    advance.addBindValue(remoteEvent.value(QStringLiteral("etag")).toString(QStringLiteral("")));
    advance.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    advance.addBindValue(calendarId);
    advance.addBindValue(providerEventId);
    advance.addBindValue(mutationId);
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM pending_mutations WHERE id=?"));
    remove.addBindValue(mutationId);
    const bool updated = seriesUpdate || update.exec();
    if (!updated || (!seriesUpdate && update.numRowsAffected() < 1 && !deleting) || !advance.exec()
        || !remove.exec() || !m_database.commit()) {
        setError(QStringLiteral("Google event update could not be reconciled"),
                 update.lastError().text() + remove.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::completeMoveMutation(const QString &mutationId, const QJsonObject &remoteEvent)
{
    const QString remoteId = remoteEvent.value(QStringLiteral("id")).toString();
    if (remoteId.isEmpty()) {
        setError(QStringLiteral("Google move response was incomplete"), QStringLiteral("missing event id"));
        return false;
    }
    if (!m_database.transaction()) return false;
    QSqlQuery mutation(m_database);
    mutation.prepare(QStringLiteral(
        "SELECT provider_event_id,payload_json FROM pending_mutations WHERE id=? AND operation='move'"));
    mutation.addBindValue(mutationId);
    if (!mutation.exec() || !mutation.next()) {
        setError(QStringLiteral("Queued move could not be found"), mutation.lastError().text());
        m_database.rollback();
        return false;
    }
    const QString providerEventId = mutation.value(0).toString();
    const QJsonObject payload = QJsonDocument::fromJson(mutation.value(1).toByteArray()).object();
    const QString targetCalendarId = payload.value(QStringLiteral("targetCalendarId")).toString();
    if (targetCalendarId.isEmpty()) {
        setError(QStringLiteral("Queued move was incomplete"), QStringLiteral("missing destination calendar"));
        m_database.rollback();
        return false;
    }
    QSqlQuery update(m_database);
    update.prepare(QStringLiteral(
        "UPDATE events SET provider_event_id=?,event_url=?,provider_uid=?,etag=?,provider_updated_at=?,"
        "status=?,transparency=?,raw_json=?,source='google' "
        "WHERE calendar_id=? AND provider_event_id=?"));
    update.addBindValue(remoteId);
    update.addBindValue(remoteEvent.value(QStringLiteral("htmlLink")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("iCalUID")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("etag")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("updated")).toString(QStringLiteral("")));
    update.addBindValue(remoteEvent.value(QStringLiteral("status")).toString(QStringLiteral("confirmed")));
    update.addBindValue(remoteEvent.value(QStringLiteral("transparency")).toString(QStringLiteral("opaque")));
    update.addBindValue(QString::fromUtf8(QJsonDocument(remoteEvent).toJson(QJsonDocument::Compact)));
    update.addBindValue(targetCalendarId);
    update.addBindValue(providerEventId);
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM pending_mutations WHERE id=?"));
    remove.addBindValue(mutationId);
    if (!update.exec() || update.numRowsAffected() < 1 || !remove.exec() || !m_database.commit()) {
        setError(QStringLiteral("Google event move could not be reconciled"),
                 update.lastError().text() + remove.lastError().text());
        m_database.rollback();
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::completeDeleteMutation(const QString &mutationId)
{
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral(
        "DELETE FROM pending_mutations WHERE id=? "
        "AND operation IN ('delete','delete-series','delete-future')"));
    remove.addBindValue(mutationId);
    if (!remove.exec() || remove.numRowsAffected() != 1) {
        setError(QStringLiteral("Google event deletion could not be reconciled"),
                 remove.lastError().text());
        return false;
    }
    m_lastError.clear();
    return true;
}

bool Database::setSyncCursor(const QString &accountId, const QString &calendarId,
                             const QString &cursor)
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO sync_cursors(account_id, calendar_id, cursor, updated_at) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(account_id, calendar_id) DO UPDATE SET cursor=excluded.cursor, updated_at=excluded.updated_at"));
    query.addBindValue(accountId);
    query.addBindValue(calendarId);
    query.addBindValue(cursor);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (query.exec())
        return true;
    setError(QStringLiteral("Sync cursor could not be saved"), query.lastError().text());
    return false;
}

bool Database::replaceGoogleCalendars(const QString &accountId, const QJsonArray &items)
{
    if (!m_database.transaction()) {
        setError(QStringLiteral("Calendar sync transaction could not start"), m_database.lastError().text());
        return false;
    }

    QSet<QString> retained;
    QSqlQuery upsert(m_database);
    upsert.prepare(QStringLiteral(
        "INSERT INTO calendars(id, name, color, source, account_id, provider_calendar_id, time_zone, access_role, selected, allowed_conference_types, default_reminders) "
        "VALUES(?, ?, ?, 'google', ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET "
        "name=excluded.name, color=excluded.color, account_id=excluded.account_id, "
        "provider_calendar_id=excluded.provider_calendar_id, time_zone=excluded.time_zone, "
        "access_role=excluded.access_role, selected=calendars.selected, "
        "allowed_conference_types=excluded.allowed_conference_types, "
        "default_reminders=excluded.default_reminders, source='google'"));

    for (const auto &value : items) {
        if (!value.isObject())
            continue;
        const QJsonObject item = value.toObject();
        const QString providerId = item.value(QStringLiteral("id")).toString();
        if (providerId.isEmpty())
            continue;
        const QString internalId = googleCalendarId(accountId, providerId);
        retained.insert(internalId);
        upsert.bindValue(0, internalId);
        upsert.bindValue(1, item.value(QStringLiteral("summaryOverride")).toString(
                                   item.value(QStringLiteral("summary")).toString(providerId)));
        upsert.bindValue(2, item.value(QStringLiteral("backgroundColor")).toString(QStringLiteral("#6c8cdb")));
        upsert.bindValue(3, accountId);
        upsert.bindValue(4, providerId);
        upsert.bindValue(5, item.value(QStringLiteral("timeZone")).toString(QStringLiteral("")));
        upsert.bindValue(6, item.value(QStringLiteral("accessRole")).toString(QStringLiteral("")));
        upsert.bindValue(7, item.value(QStringLiteral("selected")).toBool(true) ? 1 : 0);
        upsert.bindValue(8, QString::fromUtf8(QJsonDocument(
            item.value(QStringLiteral("conferenceProperties")).toObject()
                .value(QStringLiteral("allowedConferenceSolutionTypes")).toArray())
                .toJson(QJsonDocument::Compact)));
        upsert.bindValue(9, QString::fromUtf8(QJsonDocument(
            item.value(QStringLiteral("defaultReminders")).toArray())
                .toJson(QJsonDocument::Compact)));
        if (!upsert.exec()) {
            setError(QStringLiteral("Google calendar could not be stored"), upsert.lastError().text());
            m_database.rollback();
            return false;
        }
    }

    QSqlQuery existing(m_database);
    existing.prepare(QStringLiteral("SELECT id FROM calendars WHERE account_id=? AND source='google'"));
    existing.addBindValue(accountId);
    if (!existing.exec()) {
        setError(QStringLiteral("Existing Google calendars could not be read"), existing.lastError().text());
        m_database.rollback();
        return false;
    }
    QStringList removed;
    while (existing.next()) {
        const QString id = existing.value(0).toString();
        if (!retained.contains(id))
            removed.append(id);
    }
    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM calendars WHERE id=?"));
    for (const auto &id : removed) {
        remove.bindValue(0, id);
        if (!remove.exec()) {
            setError(QStringLiteral("Removed Google calendar could not be deleted"), remove.lastError().text());
            m_database.rollback();
            return false;
        }
    }

    if (!m_database.commit()) {
        setError(QStringLiteral("Calendar sync transaction could not commit"), m_database.lastError().text());
        return false;
    }
    return true;
}

QJsonDocument Database::calendarsForAccount(const QString &accountId) const
{
    QJsonArray result;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT id, provider_calendar_id, name, color, time_zone, access_role, selected, allowed_conference_types "
        "FROM calendars WHERE account_id=? ORDER BY name"));
    query.addBindValue(accountId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return QJsonDocument(result);
    }
    while (query.next()) {
        result.append(QJsonObject {
            { QStringLiteral("id"), query.value(0).toString() },
            { QStringLiteral("providerCalendarId"), query.value(1).toString() },
            { QStringLiteral("name"), query.value(2).toString() },
            { QStringLiteral("color"), query.value(3).toString() },
            { QStringLiteral("timeZone"), query.value(4).toString() },
            { QStringLiteral("accessRole"), query.value(5).toString() },
            { QStringLiteral("selected"), query.value(6).toBool() },
            { QStringLiteral("allowedConferenceTypes"),
              QJsonDocument::fromJson(query.value(7).toByteArray()).array() }
        });
    }
    return QJsonDocument(result);
}

QString Database::syncCursor(const QString &accountId, const QString &calendarId) const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT cursor FROM sync_cursors WHERE account_id=? AND calendar_id=?"));
    query.addBindValue(accountId);
    query.addBindValue(calendarId);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

bool Database::applyGoogleEvents(const QString &accountId, const QString &calendarId,
                                 const QJsonArray &items, const QString &nextSyncToken,
                                 bool fullSync)
{
    if (nextSyncToken.isEmpty()) {
        setError(QStringLiteral("Google event page is incomplete"), QStringLiteral("missing nextSyncToken"));
        return false;
    }
    if (!m_database.transaction()) {
        setError(QStringLiteral("Event sync transaction could not start"), m_database.lastError().text());
        return false;
    }

    QString calendarTimeZone;
    QSqlQuery calendarZone(m_database);
    calendarZone.prepare(QStringLiteral("SELECT time_zone FROM calendars WHERE id=?"));
    calendarZone.addBindValue(calendarId);
    if (calendarZone.exec() && calendarZone.next())
        calendarTimeZone = calendarZone.value(0).toString();

    if (fullSync) {
        QSqlQuery clear(m_database);
        clear.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND source='google'"));
        clear.addBindValue(calendarId);
        if (!clear.exec()) {
            setError(QStringLiteral("Old Google events could not be cleared"), clear.lastError().text());
            m_database.rollback();
            return false;
        }
    }

    QSqlQuery remove(m_database);
    remove.prepare(QStringLiteral("DELETE FROM events WHERE calendar_id=? AND provider_event_id=?"));
    QSqlQuery pendingDelete(m_database);
    pendingDelete.prepare(QStringLiteral(
        "SELECT 1 FROM pending_mutations WHERE calendar_id=? AND provider_event_id=? "
        "AND operation IN ('delete','move') LIMIT 1"));
    QSqlQuery insert(m_database);
    insert.prepare(QStringLiteral(
        "INSERT INTO events(provider_event_id, date_key, calendar_id, start_ms, end_ms, all_day, "
        "title, description, location, event_url, provider_uid, time_zone, status, transparency, "
        "etag, provider_updated_at, raw_json, source,all_day_start_date,all_day_end_date,"
        "recurring_event_id,original_start_ms,original_start_date,recurrence_json,is_exception) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 'google',?,?,?,?,?,?,?)"));

    for (const auto &value : items) {
        if (!value.isObject())
            continue;
        const QJsonObject item = value.toObject();
        const QString providerEventId = item.value(QStringLiteral("id")).toString();
        if (providerEventId.isEmpty())
            continue;

        remove.bindValue(0, calendarId);
        remove.bindValue(1, providerEventId);
        if (!remove.exec()) {
            setError(QStringLiteral("Existing Google event could not be replaced"), remove.lastError().text());
            m_database.rollback();
            return false;
        }
        pendingDelete.bindValue(0, calendarId);
        pendingDelete.bindValue(1, providerEventId);
        if (!pendingDelete.exec()) {
            setError(QStringLiteral("Pending event deletion could not be checked"),
                     pendingDelete.lastError().text());
            m_database.rollback();
            return false;
        }
        if (pendingDelete.next())
            continue;
        if (item.value(QStringLiteral("status")).toString() == QStringLiteral("cancelled"))
            continue;

        const QJsonObject startValue = item.value(QStringLiteral("start")).toObject();
        const QJsonObject endValue = item.value(QStringLiteral("end")).toObject();
        const bool allDay = startValue.contains(QStringLiteral("date"));
        const QString allDayStartDate = allDay ? startValue.value(QStringLiteral("date")).toString() : QStringLiteral("");
        const QString allDayEndDate = allDay ? endValue.value(QStringLiteral("date")).toString() : QStringLiteral("");
        const QString timeZone = startValue.value(QStringLiteral("timeZone")).toString(calendarTimeZone);
        const QDateTime start = googleDateTime(startValue, timeZone);
        const QDateTime end = googleDateTime(endValue, timeZone);
        const QString recurringEventId = item.value(QStringLiteral("recurringEventId")).toString(QStringLiteral(""));
        const QJsonObject originalStartValue = item.value(QStringLiteral("originalStartTime")).toObject();
        const QString originalStartDate = originalStartValue.value(QStringLiteral("date")).toString(QStringLiteral(""));
        const QDateTime originalStart = googleDateTime(originalStartValue, timeZone);
        const qint64 originalStartMs = originalStartValue.contains(QStringLiteral("dateTime")) && originalStart.isValid()
            ? originalStart.toMSecsSinceEpoch() : 0;
        const QString recurrenceJson = item.contains(QStringLiteral("recurrence"))
            ? QString::fromUtf8(QJsonDocument(item.value(QStringLiteral("recurrence")).toArray()).toJson(QJsonDocument::Compact))
            : QStringLiteral("");
        const bool isException = !recurringEventId.isEmpty()
            && ((!originalStartDate.isEmpty() && originalStartDate != allDayStartDate)
                || (originalStartMs > 0 && originalStartMs != start.toMSecsSinceEpoch()));
        if (!start.isValid() || !end.isValid() || end <= start)
            continue;

        QDate firstDate = allDay
            ? QDate::fromString(startValue.value(QStringLiteral("date")).toString(), Qt::ISODate)
            : start.toLocalTime().date();
        QDate lastDate = allDay
            ? QDate::fromString(endValue.value(QStringLiteral("date")).toString(), Qt::ISODate).addDays(-1)
            : end.addMSecs(-1).toLocalTime().date();
        for (QDate date = firstDate; date.isValid() && date <= lastDate; date = date.addDays(1)) {
            int column = 0;
            insert.bindValue(column++, providerEventId);
            insert.bindValue(column++, date.toString(Qt::ISODate));
            insert.bindValue(column++, calendarId);
            insert.bindValue(column++, start.toMSecsSinceEpoch());
            insert.bindValue(column++, end.toMSecsSinceEpoch());
            insert.bindValue(column++, allDay ? 1 : 0);
            insert.bindValue(column++, item.value(QStringLiteral("summary")).toString(QStringLiteral("")));
            insert.bindValue(column++, item.value(QStringLiteral("description")).toString(QStringLiteral("")));
            insert.bindValue(column++, item.value(QStringLiteral("location")).toString(QStringLiteral("")));
            insert.bindValue(column++, item.value(QStringLiteral("htmlLink")).toString(QStringLiteral("")));
            insert.bindValue(column++, item.value(QStringLiteral("iCalUID")).toString(QStringLiteral("")));
            insert.bindValue(column++, timeZone.isNull() ? QStringLiteral("") : timeZone);
            insert.bindValue(column++, item.value(QStringLiteral("status")).toString(QStringLiteral("")));
            insert.bindValue(column++, item.value(QStringLiteral("transparency")).toString(QStringLiteral("")));
            insert.bindValue(column++, item.value(QStringLiteral("etag")).toString(QStringLiteral("")));
            insert.bindValue(column++, item.value(QStringLiteral("updated")).toString(QStringLiteral("")));
            insert.bindValue(column++, QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact)));
            insert.bindValue(column++, allDayStartDate);
            insert.bindValue(column++, allDayEndDate);
            insert.bindValue(column++, recurringEventId);
            insert.bindValue(column++, originalStartMs);
            insert.bindValue(column++, originalStartDate);
            insert.bindValue(column++, recurrenceJson);
            insert.bindValue(column++, isException ? 1 : 0);
            if (!insert.exec()) {
                setError(QStringLiteral("Google event could not be stored"), insert.lastError().text());
                m_database.rollback();
                return false;
            }
        }
    }

    QSqlQuery cursor(m_database);
    cursor.prepare(QStringLiteral(
        "INSERT INTO sync_cursors(account_id, calendar_id, cursor, updated_at) VALUES(?, ?, ?, ?) "
        "ON CONFLICT(account_id, calendar_id) DO UPDATE SET cursor=excluded.cursor, updated_at=excluded.updated_at"));
    cursor.addBindValue(accountId);
    cursor.addBindValue(calendarId);
    cursor.addBindValue(nextSyncToken);
    cursor.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!cursor.exec()) {
        setError(QStringLiteral("Google sync cursor could not be stored"), cursor.lastError().text());
        m_database.rollback();
        return false;
    }

    if (!m_database.commit()) {
        setError(QStringLiteral("Event sync transaction could not commit"), m_database.lastError().text());
        return false;
    }
    return true;
}

QJsonDocument Database::searchEvents(const QString &queryText, int limit) const
{
    QJsonArray events;
    QString term = queryText.trimmed();
    if (term.isEmpty())
        return QJsonDocument(events);

    QMap<QString, QString> filters;
    const QRegularExpression filterPattern(
        QStringLiteral("\\b(calendar|after|before|organizer|response):(?:\"([^\"]+)\"|(\\S+))"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = filterPattern.globalMatch(term);
    QList<QPair<int, int>> spans;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        filters.insert(match.captured(1).toLower(),
                       match.captured(2).isEmpty() ? match.captured(3) : match.captured(2));
        spans.prepend({ match.capturedStart(), match.capturedLength() });
    }
    for (const auto &[start, length] : spans) term.remove(start, length);
    term = term.simplified();

    const QString displayJson = QStringLiteral(
        "COALESCE((SELECT p.payload_json FROM pending_mutations p WHERE p.calendar_id=e.calendar_id "
        "AND p.provider_event_id=e.provider_event_id AND p.operation IN ('update','update-series','update-future','rsvp') "
        "ORDER BY p.created_at DESC LIMIT 1),e.raw_json)");
    QStringList conditions {
        QStringLiteral("e.rowid IN (SELECT MIN(rowid) FROM events GROUP BY provider_event_id)"),
        QStringLiteral("c.selected=1"),
        QStringLiteral("(c.source!='compat-json' OR NOT EXISTS (SELECT 1 FROM accounts WHERE provider='google' AND last_sync_at!=''))")
    };
    QVariantList bindings;
    if (!term.isEmpty()) {
        conditions.append(QStringLiteral(
            "(instr(lower(e.title),lower(?))>0 OR instr(lower(e.description),lower(?))>0 "
            "OR instr(lower(e.location),lower(?))>0 OR instr(lower(c.name),lower(?))>0 "
            "OR instr(lower(%1),lower(?))>0)").arg(displayJson));
        for (int index = 0; index < 5; ++index) bindings.append(term);
    }
    if (filters.contains(QStringLiteral("calendar"))) {
        conditions.append(QStringLiteral("instr(lower(c.name),lower(?))>0"));
        bindings.append(filters.value(QStringLiteral("calendar")));
    }
    if (filters.contains(QStringLiteral("organizer"))) {
        conditions.append(QStringLiteral(
            "instr(lower(COALESCE(json_extract(CASE WHEN json_valid(%1) THEN %1 ELSE '{}' END,"
            "'$.organizer.email'),'')),lower(?))>0").arg(displayJson));
        bindings.append(filters.value(QStringLiteral("organizer")));
    }
    if (filters.contains(QStringLiteral("response"))) {
        conditions.append(QStringLiteral("instr(lower(%1),lower(?))>0").arg(displayJson));
        bindings.append(QStringLiteral("\"responseStatus\":\"")
                        + filters.value(QStringLiteral("response")) + QStringLiteral("\""));
    }
    const auto addDateFilter = [&](const QString &key, const QString &comparison) -> bool {
        if (!filters.contains(key)) return true;
        const QDate date = QDate::fromString(filters.value(key), Qt::ISODate);
        if (!date.isValid()) return false;
        conditions.append(QStringLiteral("e.start_ms%1?").arg(comparison));
        bindings.append(QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch());
        return true;
    };
    if (!addDateFilter(QStringLiteral("after"), QStringLiteral(">="))
        || !addDateFilter(QStringLiteral("before"), QStringLiteral("<")))
        return QJsonDocument(events);

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT e.provider_event_id, e.calendar_id, c.name AS calendar_name, c.color, e.date_key, "
        "e.start_ms, e.end_ms, e.all_day, e.title, e.description, e.location, e.event_url,"
        "%1 AS display_json,e.transparency, "
        "COALESCE(NULLIF(e.time_zone,''),c.time_zone) AS time_zone, e.etag, e.source,e.all_day_start_date,e.all_day_end_date, "
        "e.recurring_event_id,e.original_start_ms,e.original_start_date,e.recurrence_json,e.is_exception, "
        "(SELECT COUNT(*) FROM events span WHERE span.calendar_id=e.calendar_id "
        "AND span.provider_event_id=e.provider_event_id) AS day_count "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE %2 "
        "ORDER BY CASE WHEN e.end_ms>=? THEN 0 ELSE 1 END, "
        "CASE WHEN e.end_ms>=? THEN e.start_ms END ASC, "
        "CASE WHEN e.end_ms<? THEN e.start_ms END DESC LIMIT ?")
        .arg(displayJson, conditions.join(QStringLiteral(" AND "))));
    for (const QVariant &binding : bindings) query.addBindValue(binding);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(qBound(1, limit, 200));
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return QJsonDocument(events);
    }
    while (query.next())
        events.append(eventFromQuery(query));
    return QJsonDocument(events);
}

QJsonDocument Database::nextEvent() const
{
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "SELECT e.provider_event_id, e.calendar_id, c.name AS calendar_name, c.color, e.date_key, "
        "e.start_ms, e.end_ms, e.all_day, e.title, e.description, e.location, e.event_url,"
        "COALESCE((SELECT p.payload_json FROM pending_mutations p WHERE p.calendar_id=e.calendar_id "
        "AND p.provider_event_id=e.provider_event_id AND p.operation IN ('update','update-series','update-future','rsvp') "
        "ORDER BY p.created_at DESC LIMIT 1),e.raw_json) AS display_json,e.transparency, "
        "COALESCE(NULLIF(e.time_zone,''),c.time_zone) AS time_zone, e.etag, e.source,e.all_day_start_date,e.all_day_end_date, "
        "e.recurring_event_id,e.original_start_ms,e.original_start_date,e.recurrence_json,e.is_exception, "
        "(SELECT COUNT(*) FROM events span WHERE span.calendar_id=e.calendar_id "
        "AND span.provider_event_id=e.provider_event_id) AS day_count "
        "FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE e.all_day=0 AND e.end_ms>? AND c.selected=1 "
        "AND (c.source!='compat-json' OR NOT EXISTS ("
        "SELECT 1 FROM accounts WHERE provider='google' AND last_sync_at!='')) "
        "ORDER BY e.start_ms LIMIT 1"));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (query.exec() && query.next())
        return QJsonDocument(eventFromQuery(query));
    return QJsonDocument(QJsonObject {});
}

QJsonDocument Database::status() const
{
    QSqlQuery schema(QStringLiteral("SELECT COALESCE(MAX(version),0) FROM schema_migrations"), m_database);
    const int schemaVersion = schema.next() ? schema.value(0).toInt() : 0;
    QSqlQuery counts(QStringLiteral(
        "SELECT (SELECT COUNT(*) FROM calendars c WHERE c.source!='compat-json' OR NOT EXISTS ("
        "SELECT 1 FROM accounts WHERE provider='google' AND last_sync_at!='')), "
        "(SELECT COUNT(*) FROM events e JOIN calendars c ON c.id=e.calendar_id "
        "WHERE c.selected=1 AND (c.source!='compat-json' OR NOT EXISTS ("
        "SELECT 1 FROM accounts WHERE provider='google' AND last_sync_at!=''))), "
        "(SELECT COUNT(*) FROM accounts), "
        "(SELECT COUNT(*) FROM pending_mutations), "
        "(SELECT MAX(value) FROM metadata WHERE key='compat_feed_synced_at')"), m_database);
    QJsonObject result {
        { QStringLiteral("schemaVersion"), schemaVersion },
        { QStringLiteral("databasePath"), m_path },
        { QStringLiteral("lastError"), m_lastError }
    };
    if (counts.next()) {
        result.insert(QStringLiteral("calendarCount"), counts.value(0).toInt());
        result.insert(QStringLiteral("eventCount"), counts.value(1).toInt());
        result.insert(QStringLiteral("accountCount"), counts.value(2).toInt());
        result.insert(QStringLiteral("pendingMutationCount"), counts.value(3).toInt());
        result.insert(QStringLiteral("feedSyncedAt"), counts.value(4).toString());
    }
    return QJsonDocument(result);
}
