// Application entry point (spec §7, §13). Parses --sim/--real/--db, builds the
// composition root and runs the Qt event loop. Shutdown is ordered on
// aboutToQuit: lifecycle (clear M42/M106-M111, stop heartbeat) -> gateway ->
// database -> vision (spec §13). M100 is never auto-cleared.

#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QString>

#include "app/application.h"
#include "app/configuration.h"
#include "common/version.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PLC-HLM"));
    QApplication::setApplicationVersion(QString::fromLatin1(hlm::versionString()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("PLC 调宽上位机 (Inovance H3U over Modbus RTU)"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption simOption(QStringLiteral("sim"),
                                 QStringLiteral("use the in-process simulated PLC gateway (default)"));
    QCommandLineOption realOption(QStringLiteral("real"),
                                  QStringLiteral("use the real Modbus RTU gateway over the serial port"));
    QCommandLineOption dbOption(QStringLiteral("db"),
                                QStringLiteral("override the database path"), QStringLiteral("path"));
    parser.addOption(simOption);
    parser.addOption(realOption);
    parser.addOption(dbOption);
    parser.process(app);

    hlm::AppConfig cfg;
    if (parser.isSet(realOption))
        cfg.useSimulatedGateway = false;
    else if (parser.isSet(simOption))
        cfg.useSimulatedGateway = true;
    if (parser.isSet(dbOption)) {
        cfg.databasePath = parser.value(dbOption);
    } else {
        // Default machine-level data directory (spec §12): %ProgramData%\PLC-HLM\
        // on Windows, ~/.local/share/PLC-HLM/ elsewhere.
        const QString dataDir = QStandardPaths::writableLocation(
            QStandardPaths::AppDataLocation);
        if (!dataDir.isEmpty())
            cfg.databasePath = dataDir + QStringLiteral("/app.db");
    }

    hlm::Application application(cfg);
    application.start();

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &application,
                     &hlm::Application::shutdown);

    return app.exec();
}
