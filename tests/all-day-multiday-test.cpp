#include "database.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimeZone>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "usage: all-day-multiday-test TIMEZONE TIMED_DAYS FIRST_DATE\n");
        return 1;
    }
    qputenv("TZ", QByteArray(argv[1]));
    tzset();
    const int expectedTimedDays = QString::fromLocal8Bit(argv[2]).toInt();
    const QDate expectedFirstDate = QDate::fromString(QString::fromLocal8Bit(argv[3]), Qt::ISODate);
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    Database database(temporary.filePath("calendar.db"));
    const QString accountId = QStringLiteral("google:all-day-test");
    if (!temporary.isValid() || !database.open()
        || !database.upsertAccount(accountId, "google", "all-day-test", "All-day Test",
                                   "dates@example.com", "connected")
        || !database.replaceGoogleCalendars(accountId, QJsonArray { QJsonObject {
            { "id", "primary@example.com" }, { "summary", "Primary" },
            { "timeZone", "America/New_York" }, { "accessRole", "owner" }, { "selected", true }
        } })) return 2;
    const QString calendarId = database.calendarsForAccount(accountId).array().at(0).toObject()
                                   .value("id").toString();
    const QJsonObject remote {
        { "id", "spring-forward" }, { "summary", "Conference" },
        { "start", QJsonObject { { "date", "2026-03-07" } } },
        { "end", QJsonObject { { "date", "2026-03-10" } } },
        { "etag", "dates-etag" }, { "status", "confirmed" }
    };
    if (!database.applyGoogleEvents(accountId, calendarId, QJsonArray { remote }, "dates-sync", true))
        return 3;
    const QJsonArray span = database.eventsForRange("2026-03-07", "2026-03-10").array();
    if (span.size() != 3) return 4;
    for (const auto &value : span) {
        const QJsonObject event = value.toObject();
        if (!event.value("allDay").toBool() || !event.value("multiDay").toBool()
            || event.value("allDayStartDate") != QStringLiteral("2026-03-07")
            || event.value("allDayEndDate") != QStringLiteral("2026-03-10")) return 5;
    }

    const QTimeZone zone("America/New_York");
    const qint64 movedStart = QDateTime(QDate(2026, 3, 8), QTime(0, 0), zone).toMSecsSinceEpoch();
    const qint64 movedEnd = QDateTime(QDate(2026, 3, 11), QTime(0, 0), zone).toMSecsSinceEpoch();
    if (movedEnd - movedStart != 71LL * 60 * 60 * 1000) return 6;
    if (!database.updatePendingEvent(QJsonObject {
        { "id", "spring-forward" }, { "calendarId", calendarId }, { "title", "Conference" },
        { "startMs", double(movedStart) }, { "endMs", double(movedEnd) }, { "allDay", true },
        { "allDayStartDate", "2026-03-08" }, { "allDayEndDate", "2026-03-11" },
        { "timeZone", "America/New_York" }
    })) {
        std::fprintf(stderr, "update failed: %s\n", qPrintable(database.lastError()));
        return 7;
    }
    const QJsonObject mutation = database.nextPendingMutation(accountId).object();
    const QJsonObject payload = mutation.value("payload").toObject();
    if (payload.value("allDayStartDate") != QStringLiteral("2026-03-08")
        || payload.value("allDayEndDate") != QStringLiteral("2026-03-11")
        || database.eventsForRange("2026-03-08", "2026-03-11").array().size() != 3) return 8;

    QJsonObject rebasedRemote = remote;
    rebasedRemote.insert(QStringLiteral("description"), QStringLiteral("Added on another device"));
    rebasedRemote.insert(QStringLiteral("etag"), QStringLiteral("dates-etag-2"));
    rebasedRemote.insert(QStringLiteral("updated"), QStringLiteral("2026-03-01T12:00:00Z"));
    if (!database.rebaseUpdateMutation(mutation.value("id").toString(), rebasedRemote)) {
        qCritical().noquote() << database.lastError();
        return 9;
    }
    const QJsonArray rebasedRows = database.eventsForRange("2026-03-08", "2026-03-11").array();
    if (rebasedRows.size() != 3
        || rebasedRows.at(0).toObject().value("description") != QStringLiteral("Added on another device")
        || rebasedRows.at(0).toObject().value("allDayStartDate") != QStringLiteral("2026-03-08")
        || rebasedRows.at(0).toObject().value("allDayEndDate") != QStringLiteral("2026-03-11")) return 10;

    const qint64 timedStart = QDateTime(QDate(2026, 3, 9), QTime(22, 0), zone).toMSecsSinceEpoch();
    const qint64 timedEnd = QDateTime(QDate(2026, 3, 10), QTime(4, 0), zone).toMSecsSinceEpoch();
    if (!database.updatePendingEvent(QJsonObject {
        { "id", "spring-forward" }, { "calendarId", calendarId }, { "title", "Overnight" },
        { "startMs", double(timedStart) }, { "endMs", double(timedEnd) }, { "allDay", false },
        { "timeZone", "America/New_York" }
    })) return 11;
    const QJsonArray timedRows = database.eventsForRange("2026-03-09", "2026-03-10").array();
    // Timed events span display dates in the system zone. This New York
    // overnight event falls entirely on March 10 in UTC and Tokyo.
    if (timedRows.size() != expectedTimedDays) {
        std::fprintf(stderr, "%s: expected %d timed rows, got %lld\n", argv[1],
                     expectedTimedDays, static_cast<long long>(timedRows.size()));
        return 12;
    }
    for (int index = 0; index < timedRows.size(); ++index) {
        const QJsonObject event = timedRows.at(index).toObject();
        if (event.value("allDay").toBool()
            || event.value("multiDay").toBool() != (expectedTimedDays > 1)
            || event.value("dateKey").toString() != expectedFirstDate.addDays(index).toString(Qt::ISODate))
            return 13;
    }
    return 0;
}
