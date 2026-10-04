#include "calendarservice.h"
#include "database.h"
#include "googleauth.h"
#include "googlesync.h"
#include "googlemutations.h"
#include "../app/version.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("omarchy-calendar-service"));
    app.setApplicationVersion(QStringLiteral(OMARCHY_CALENDAR_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Omarchy Calendar local data service"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({ QStringLiteral("import-only"), QStringLiteral("Import the compatibility feed, print status, and exit.") });
    parser.process(app);

    const QString databasePath = qEnvironmentVariable(
        "OMARCHY_CALENDAR_DB_PATH",
        QDir::homePath() + QStringLiteral("/.local/share/omarchy-calendar/calendar.db"));
    const QString feedPath = qEnvironmentVariable(
        "OMARCHY_CALENDAR_FEED_PATH",
        QDir::homePath() + QStringLiteral("/.local/state/omarchy/calendar-events.json"));

    Database database(databasePath);
    if (!database.open()) {
        qCritical().noquote() << database.lastError();
        return EXIT_FAILURE;
    }
    // A first installation has no legacy feed. Explicit imports and existing
    // feeds must still be validated instead of silently ignoring invalid data.
    if ((parser.isSet(QStringLiteral("import-only")) || QFileInfo::exists(feedPath))
        && !database.importCompatibilityFeed(feedPath)) {
        qCritical().noquote() << database.lastError();
        return EXIT_FAILURE;
    }

    if (parser.isSet(QStringLiteral("import-only"))) {
        qInfo().noquote() << database.status().toJson(QJsonDocument::Compact);
        return EXIT_SUCCESS;
    }

    GoogleAuth googleAuth(database);
    GoogleSync googleSync(database);
    GoogleMutations googleMutations(database);
    CalendarService service(database, googleAuth, googleSync, googleMutations, feedPath);
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("org.omarchy.Calendar"))) {
        qCritical().noquote() << "Could not register org.omarchy.Calendar:" << bus.lastError().message();
        return EXIT_FAILURE;
    }
    if (!bus.registerObject(QStringLiteral("/org/omarchy/Calendar"), &service,
                            QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        qCritical().noquote() << "Could not register calendar DBus object:" << bus.lastError().message();
        return EXIT_FAILURE;
    }

    return app.exec();
}
