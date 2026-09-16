// PLC-HMI-004 developer regression tests (D7) through the real composition
// root: enumeration purity (explicit action only, no gateway disturbance),
// duplicate/in-flight save visible rejection, and real-gateway rebuild gating
// (rebuild only after the matching successful batch result; failure keeps the
// active gateway and the committed settings).
//
// These complement (never replace) the independent black-box tests; the
// database write lock provides the in-flight/failure injection that the
// frozen surface cannot hold open.

#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVector>

#include "adapters/modbus/qt_modbus_plc_gateway.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "domain/serial_connection_settings.h"
#include "domain/serial_port_descriptor.h"
#include "ports/iplc_gateway.h"
#include "ports/iserial_port_discovery.h"
#include "ports/repositories.h"
#include "ui/MainWindow.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

SerialPortDescriptor descriptorFor(const QString &portName)
{
    SerialPortDescriptor descriptor;
    descriptor.port_name = portName;
    return descriptor;
}

bool comboHasPort(QComboBox *combo, const QString &portName)
{
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemText(i).contains(portName))
            return true;
    }
    return false;
}

class ScriptedSerialPortDiscovery : public ISerialPortDiscovery
{
    Q_OBJECT

public:
    quint64 enumerateAvailablePorts() override
    {
        ++requests;
        last_request_id = next_id;
        return next_id++;
    }

    void completeWith(quint64 requestId,
                      const QVector<SerialPortDescriptor> &ports,
                      const QString &error = QString())
    {
        SerialEnumerationResult result;
        result.enumeration_request_id = requestId;
        result.discovered_descriptors = ports;
        result.completion_error = error;
        emit enumerationCompleted(result);
    }

    int requests = 0;
    quint64 last_request_id = 0;

private:
    quint64 next_id = 1;
};

struct StartedApp
{
    QTemporaryDir dir;
    AppConfig cfg;

    StartedApp()
    {
        cfg.useSimulatedGateway = true;
        cfg.simulatedTickIntervalMs = 0;
        cfg.databasePath = dir.filePath(QStringLiteral("app.db"));
    }

    QString databasePath() const
    {
        return dir.filePath(QStringLiteral("app.db"));
    }
};

void startAndWaitReady(Application &app, DatabaseService *&db)
{
    app.start();
    db = app.database();
    QVERIFY(db != nullptr);
    QSignalSpy readySpy(db, &DatabaseService::recipesLoaded);
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);
}

void createInitialAdmin(DatabaseService *db)
{
    QSignalSpy adminSpy(db, &DatabaseService::initialAdminCreated);
    QVERIFY(QMetaObject::invokeMethod(
        db, "createInitialAdmin", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.count() > 0, 10000);
    QCOMPARE(adminSpy[0][0].toBool(), true);
}

void loginAsAdmin(DatabaseService *db)
{
    createInitialAdmin(db);
    QSignalSpy loginSpy(db, &DatabaseService::loginResult);
    QVERIFY(QMetaObject::invokeMethod(
        db, "login", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.count() > 0, 10000);
    QVERIFY(loginSpy[0][0].value<LoginResult>().ok);
}

UsersSettingsPage *settingsPageOf(Application &app)
{
    return app.window()->findChild<UsersSettingsPage *>();
}

// Holds the SQLite write lock so a save is genuinely in flight / fails.
class DatabaseWriteLock
{
public:
    explicit DatabaseWriteLock(const QString &databasePath)
        : database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                             QStringLiteral("serial-dev-locker")))
    {
        database.setDatabaseName(databasePath);
    }

    ~DatabaseWriteLock() { release(); }

    bool acquire()
    {
        if (!database.open())
            return false;
        query = QSqlQuery(database);
        held = query.exec(QStringLiteral("BEGIN EXCLUSIVE"));
        return held;
    }

    void release()
    {
        if (released)
            return;
        released = true;
        if (held)
            query.exec(QStringLiteral("ROLLBACK"));
        held = false;
        query = QSqlQuery();
        if (database.isValid())
            database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(QStringLiteral("serial-dev-locker"));
    }

    bool held = false;

private:
    bool released = false;
    QSqlDatabase database;
    QSqlQuery query;
};

QString storedValue(const QString &databasePath, const QString &key)
{
    const QString connection = QStringLiteral("serial-dev-probe");
    QString value;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          connection);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                "SELECT typed_value FROM app_settings WHERE key = ?"));
            query.addBindValue(key);
            if (query.exec() && query.next())
                value = query.value(0).toString();
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return value;
}

} // namespace

class SerialSettingsApplicationDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void enumerationIsExplicitPassiveAndCorrelated();
    void duplicateInFlightSaveIsVisiblyRejected();
    void realGatewayRebuildHappensOnlyAfterTheMatchingSuccess();
};

// --- enumeration purity -------------------------------------------------------

void SerialSettingsApplicationDeveloperTest::enumerationIsExplicitPassiveAndCorrelated()
{
    StartedApp started;
    ScriptedSerialPortDiscovery discovery;
    started.cfg.serialPortDiscovery = &discovery;

    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);

    auto *gw = qobject_cast<SimulatedPlcGateway *>(app.gateway());
    QVERIFY(gw != nullptr);
    for (int i = 0; i < 10 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());
    const quint64 generation = app.gateway()->gatewayGeneration();
    auto *page = settingsPageOf(app);
    QVERIFY(page != nullptr);

    // No administrator action yet: no enumeration, no probing, no gateway
    // change.
    QCOMPARE(discovery.requests, 0);

    emit page->enumerateSerialPortsRequested();
    QTRY_COMPARE_WITH_TIMEOUT(discovery.requests, 1, 5000);
    QCOMPARE(app.gateway()->gatewayGeneration(), generation);
    QVERIFY(gw->isOnline());

    // An unmatched completion cannot update the page; the matching one can,
    // still without touching the active gateway.
    discovery.completeWith(discovery.last_request_id + 1000,
                           {descriptorFor(QStringLiteral("COM9"))});
    QTest::qWait(50);
    QVERIFY(!comboHasPort(page->serialPortComboBox(), QStringLiteral("COM9")));
    discovery.completeWith(discovery.last_request_id,
                           {descriptorFor(QStringLiteral("COM1")),
                            descriptorFor(QStringLiteral("COM3"))});
    QTRY_VERIFY_WITH_TIMEOUT(
        comboHasPort(page->serialPortComboBox(), QStringLiteral("COM3")), 5000);
    QCOMPARE(app.gateway()->gatewayGeneration(), generation);
    QVERIFY(gw->isOnline());

    // The boundary stays passive: no further request is issued on its own.
    QTest::qWait(100);
    QCOMPARE(discovery.requests, 1);

    app.shutdown();
}

// --- duplicate in-flight save -------------------------------------------------

void SerialSettingsApplicationDeveloperTest::duplicateInFlightSaveIsVisiblyRejected()
{
    StartedApp started;
    ScriptedSerialPortDiscovery discovery;
    started.cfg.serialPortDiscovery = &discovery;

    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);
    loginAsAdmin(db);
    app.coordinator()->setRole(Role::Admin);

    auto *page = settingsPageOf(app);
    QVERIFY(page != nullptr);

    QVector<SettingsBatchResult> results;
    connect(db, &DatabaseService::serialSettingsBatchSaved, this,
            [&results](const SettingsBatchResult &result) { results.append(result); });

    DatabaseWriteLock lock(started.databasePath());
    QVERIFY(lock.acquire());

    SerialConnectionSettings first;
    first.port_name = QStringLiteral("COM5");
    SerialConnectionSettings second;
    second.port_name = QStringLiteral("COM6");

    emit page->saveSerialSettingsRequested(first);
    const QString pending = page->serialSettingsStatusText();
    QVERIFY2(!pending.trimmed().isEmpty(), "the accepted save must be pending visibly");

    // The accepted save is blocked on the database lock: the duplicate must be
    // rejected immediately and visibly, without a second batch result.
    emit page->saveSerialSettingsRequested(second);
    const QString rejected = page->serialSettingsStatusText();
    QVERIFY2(!rejected.trimmed().isEmpty() && rejected != pending,
             "an in-flight duplicate save must be visibly rejected");
    QCOMPARE(results.size(), 0);

    lock.release();
    QTRY_VERIFY_WITH_TIMEOUT(results.size() >= 1, 30000);
    QCOMPARE(results.size(), 1);
    QVERIFY2(results[0].committed, qPrintable(results[0].error));
    QVERIFY(results[0].batch_id != 0);
    QCOMPARE(storedValue(started.databasePath(), QStringLiteral("serial.comPort")),
             QStringLiteral("COM5"));
    QCOMPARE(page->serialSettings().port_name, QStringLiteral("COM5"));
    QVERIFY2(!page->serialSettingsStatusText().contains(QStringLiteral("失败")),
             "the accepted save must remain the visible terminal result");

    // The rejection does not latch: a later explicit save succeeds.
    emit page->saveSerialSettingsRequested(second);
    QTRY_VERIFY_WITH_TIMEOUT(results.size() >= 2, 20000);
    QVERIFY2(results.last().committed, qPrintable(results.last().error));
    QTRY_COMPARE_WITH_TIMEOUT(
        storedValue(started.databasePath(), QStringLiteral("serial.comPort")),
        QStringLiteral("COM6"), 10000);

    app.shutdown();
}

// --- real-gateway rebuild gating ----------------------------------------------

void SerialSettingsApplicationDeveloperTest::realGatewayRebuildHappensOnlyAfterTheMatchingSuccess()
{
    StartedApp started;
    started.cfg.useSimulatedGateway = false;
    ScriptedSerialPortDiscovery discovery;
    started.cfg.serialPortDiscovery = &discovery;

    Application app(started.cfg);
    IPlcGateway *initialGateway = app.gateway();
    QVERIFY(initialGateway != nullptr);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);

    // Startup applied the persisted (default) settings and replaced the
    // constructor gateway before it started (spec §13 ordering).
    QTRY_VERIFY_WITH_TIMEOUT(app.gateway() != initialGateway, 5000);
    auto *gateway = qobject_cast<QtModbusPlcGateway *>(app.gateway());
    QVERIFY(gateway != nullptr);
    QCOMPARE(gateway->configuration().portName, QStringLiteral("COM1"));
    const quint64 generationBefore = gateway->gatewayGeneration();

    auto *page = settingsPageOf(app);
    QVERIFY(page != nullptr);

    QVector<SettingsBatchResult> results;
    connect(db, &DatabaseService::serialSettingsBatchSaved, this,
            [&results](const SettingsBatchResult &result) { results.append(result); });

    // Success path: while the batch is in flight (write lock held) the active
    // gateway must not be stopped, replaced or advanced.
    DatabaseWriteLock lock(started.databasePath());
    QVERIFY(lock.acquire());
    SerialConnectionSettings replacement;
    replacement.port_name = QStringLiteral("COM247");
    replacement.baud_rate = 19200;
    emit page->saveSerialSettingsRequested(replacement);
    QTest::qWait(300);
    QCOMPARE(app.gateway(), static_cast<IPlcGateway *>(gateway));
    QCOMPARE(app.gateway()->gatewayGeneration(), generationBefore);
    QCOMPARE(results.size(), 0);

    lock.release();
    QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 30000);
    QVERIFY2(results[0].committed, qPrintable(results[0].error));
    QTRY_VERIFY_WITH_TIMEOUT(app.gateway() != static_cast<IPlcGateway *>(gateway), 5000);
    auto *rebuilt = qobject_cast<QtModbusPlcGateway *>(app.gateway());
    QVERIFY(rebuilt != nullptr);
    QCOMPARE(rebuilt->configuration().portName, QStringLiteral("COM247"));
    QCOMPARE(rebuilt->configuration().baudRate, 19200);
    QCOMPARE(rebuilt->configuration().station, quint8(1));
    QCOMPARE(rebuilt->gatewayGeneration(), generationBefore + 1);

    // Failure path: the rebuilt gateway and the committed settings stay
    // unchanged; the failure is a correlated terminal result with an error.
    const quint64 generationAfterSuccess = app.gateway()->gatewayGeneration();
    DatabaseWriteLock failingLock(started.databasePath());
    QVERIFY(failingLock.acquire());
    SerialConnectionSettings failing;
    failing.port_name = QStringLiteral("COM248");
    emit page->saveSerialSettingsRequested(failing);
    QTRY_VERIFY_WITH_TIMEOUT(results.size() >= 2, 30000);
    const SettingsBatchResult failed = results.last();
    QVERIFY(!failed.committed);
    QVERIFY(!failed.error.isEmpty());
    QCOMPARE(failed.batch_id != 0, true);
    failingLock.release();

    QCOMPARE(app.gateway(), static_cast<IPlcGateway *>(rebuilt));
    QCOMPARE(app.gateway()->gatewayGeneration(), generationAfterSuccess);
    QCOMPARE(storedValue(started.databasePath(), QStringLiteral("serial.comPort")),
             QStringLiteral("COM247"));

    app.shutdown();
}

QTEST_MAIN(SerialSettingsApplicationDeveloperTest)
#include "serial_settings_application_developer_test.moc"
