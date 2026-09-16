// PLC-HMI-004 black-box integration tests: passive serial discovery and the
// atomic, correlated serial-settings batch through the real composition root
// (brief OB-4..OB-10).
//
// Authored only from the sanitized behavior brief .ai/test-briefs/PLC-HMI-004.yaml
// and the approved .ai/project-contract.yaml. No production implementation
// source was read.
//
// Frozen surface used (author assumptions recorded in the author-phase RED
// report; contract-literal names plus existing inspectable APIs):
//   AppConfig::serialPortDiscovery  (injected ISerialPortDiscovery*, the
//                                    "fake/injected discovery" seam)
//   hlm::ISerialPortDiscovery::enumerateAvailablePorts()
//   hlm::SerialEnumerationResult { enumeration_request_id,
//                                  discovered_descriptors, completion_error }
//   hlm::ISerialPortDiscovery::enumerationCompleted(const SerialEnumerationResult&)
//   hlm::SettingsBatchResult { batch_id, committed, error } announced by
//     DatabaseService::serialSettingsBatchSaved
//   UsersSettingsPage serial surface (see serial_settings_page_test.cpp)
//   Existing: Application, AppConfig, DatabaseService, SimulatedPlcGateway,
//   ShellModel/Role, MainWindow.
//
// Scope notes (recorded as coverage gaps in the RED report):
//  - The real QtSerialPortDiscovery adapter, real COM hardware and the passive
//    "no probe" property cannot be exercised in this headless environment; the
//    injected fake drives the composition-root wiring and passivity instead.
//  - With useSimulatedGateway the real serial gateway is not composed, so
//    "persisted settings applied before the real gateway starts" is covered by
//    the fresh-defaults and restart-echo observables.
//
// Expected RED: compile failure against the current tree; the discovery
// interface, batch result and page serial surface do not exist yet.

#include <QtTest>

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVector>
#include <type_traits>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "domain/serial_connection_settings.h"
#include "domain/serial_port_descriptor.h"
#include "ports/iserial_port_discovery.h"
#include "ports/repositories.h"
#include "ui/MainWindow.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// The documented neutral default of the `parity` field, representation-agnostic
// (see serial_connection_settings_test.cpp).
template <typename T>
bool isNeutralParity(const T &value)
{
    if constexpr (requires { value.has_value(); }) {
        if (!value.has_value())
            return true;
        return isNeutralParity(*value);
    } else if constexpr (std::is_enum_v<T>) {
        return static_cast<std::underlying_type_t<T>>(value) == 0;
    } else if constexpr (std::is_integral_v<T>) {
        return value == 0;
    } else if constexpr (std::is_same_v<std::decay_t<T>, QString>) {
        return value.isEmpty()
            || value.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
            || value.compare(QStringLiteral("无"), Qt::CaseInsensitive) == 0;
    } else {
        return value == T{};
    }
}

// Assign a parity that is observably not the neutral default
// (representation-agnostic; see serial_connection_settings_test.cpp).
template <typename T>
void setAlternateParity(T &value)
{
    if constexpr (requires {
                      typename T::value_type;
                      value.emplace(static_cast<typename T::value_type>(1));
                  }) {
        value.emplace(static_cast<typename T::value_type>(1));
    } else if constexpr (std::is_enum_v<T>) {
        value = static_cast<T>(static_cast<std::underlying_type_t<T>>(value) + 1);
    } else if constexpr (std::is_integral_v<T>) {
        value = (value == 0 ? 1 : 0);
    } else if constexpr (std::is_same_v<std::decay_t<T>, QString>) {
        value = QStringLiteral("even");
    } else {
        value = T{};
    }
}

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

// --- injected discovery -------------------------------------------------------

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

// --- application harness ------------------------------------------------------

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
    // `isRestricted()` is false before the worker opens the database, so wait
    // for the startup listRecipes() that only runs on the ready() path.
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

// Explicit first-run administrator creation followed by the login assertion,
// following the established integration-test convention
// (tests/integration/recipe_database_routing_developer_test.cpp). The contract
// forbids default credentials and an auto-created administrator, so a case on a
// fresh temporary database must create the initial administrator explicitly
// before logging in. `adminAlreadyCreated` is true only for the restart case,
// where the administrator was explicitly created by the previous session and
// persists in the same database; the login assertion is identical either way.
void loginAsAdmin(DatabaseService *db, bool adminAlreadyCreated = false)
{
    if (!adminAlreadyCreated)
        createInitialAdmin(db);

    QSignalSpy loginSpy(db, &DatabaseService::loginResult);
    QVERIFY(QMetaObject::invokeMethod(
        db, "login", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.count() > 0, 10000);
    QVERIFY(loginSpy[0][0].value<LoginResult>().ok);
}

SimulatedPlcGateway *gatewayOf(Application &app)
{
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app.gateway());
    return gw;
}

UsersSettingsPage *settingsPageOf(Application &app)
{
    return app.window()->findChild<UsersSettingsPage *>();
}

// --- database observation helpers --------------------------------------------

bool hasKeyValueColumns(QSqlDatabase &database, const QString &table)
{
    bool hasKey = false;
    bool hasValue = false;
    QSqlQuery columns(database);
    if (!columns.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table)))
        return false;
    while (columns.next()) {
        const QString name = columns.value(1).toString().toLower();
        hasKey = hasKey || name == QStringLiteral("key");
        hasValue = hasValue || name == QStringLiteral("value");
    }
    return hasKey && hasValue;
}

// Every non-empty text value of the settings-like tables (a table with both a
// key and a value column is preferred; otherwise every user table is scanned).
// The settings table stores the serial keys as its rows; the probe intentionally
// does not depend on any key name.
QStringList storedTextValues(QSqlDatabase &database)
{
    QStringList values;
    QStringList tables;
    QSqlQuery tableQuery(database);
    if (!tableQuery.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%'")))
        return values;
    while (tableQuery.next())
        tables.append(tableQuery.value(0).toString());

    QStringList keyValueTables;
    for (const QString &table : tables) {
        if (hasKeyValueColumns(database, table))
            keyValueTables.append(table);
    }
    const QStringList scan = keyValueTables.isEmpty() ? tables : keyValueTables;

    for (const QString &table : scan) {
        QSqlQuery rows(database);
        if (!rows.exec(QStringLiteral("SELECT * FROM %1").arg(table)))
            continue;
        while (rows.next()) {
            const QSqlRecord record = rows.record();
            for (int i = 0; i < record.count(); ++i) {
                const QVariant cell = rows.value(i);
                if (!cell.isValid() || cell.isNull())
                    continue;
                const QString text = cell.toString();
                if (!text.isEmpty())
                    values.append(text);
            }
        }
    }
    return values;
}

QStringList storedTextValues(const QString &databasePath)
{
    const QString connectionName = QStringLiteral("serial-settings-probe");
    QStringList values;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            values = storedTextValues(database);
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return values;
}

bool valuesContain(const QString &databasePath, const QString &needle)
{
    return storedTextValues(databasePath).contains(needle);
}

// Injects a database failure by holding the SQLite write lock for the duration
// of one save; RAII guarantees the lock is released even when an assertion
// aborts the test.
class DatabaseWriteLock
{
public:
    explicit DatabaseWriteLock(const QString &databasePath)
        : database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                             QStringLiteral("serial-settings-locker")))
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

    // The held connection is exposed so the test can read the protected
    // database through the same connection while the write lock is held.
    QSqlDatabase &connection() { return database; }

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
        QSqlDatabase::removeDatabase(QStringLiteral("serial-settings-locker"));
    }

    bool held = false;

private:
    bool released = false;
    QSqlDatabase database;
    QSqlQuery query;
};

} // namespace

class SerialSettingsApplicationTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-10: fresh defaults, no administrative action ---------------------
    void freshStartupPublishesDocumentedDefaultsWithoutAdministratorAction();

    // --- OB-3 / OB-4: passive correlated enumeration -------------------------
    void enumerationIsPassiveAndCorrelatedThroughTheCompositionRoot();
    void unmatchedEnumerationCompletionDoesNotAffectThePage();
    void missingSavedPortIsReportedThroughTheCompositionRootAndNotReplaced();

    // --- OB-5 / OB-6 / OB-7: atomic save, one correlated result -------------
    void saveCommitsAllSevenKeysWithOneCorrelatedResult();
    void duplicateInFlightSaveIsRejectedAndCannotReplaceTheAcceptedSave();

    // --- OB-5 / OB-7 / OB-8: failure leaves committed state untouched --------
    void failedBatchLeavesCommittedValuesAndGatewayUntouchedAndALaterSaveSucceeds();

    // --- OB-5 / OB-10: every one of the seven keys persists and echoes -------
    void committedBatchEchoesAllSevenKeysAfterRestart();
    void failedBatchLeavesAllSevenKeysUnchangedAcrossRestart();

    // --- OB-10: persisted settings echo after restart ------------------------
    void persistedSettingsAreEchoedAfterRestart();
};

// --- OB-10 --------------------------------------------------------------------

void SerialSettingsApplicationTest::freshStartupPublishesDocumentedDefaultsWithoutAdministratorAction()
{
    StartedApp started;
    ScriptedSerialPortDiscovery discovery;
    started.cfg.serialPortDiscovery = &discovery;

    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);

    // No login, no role and no page interaction happen here: nothing may be
    // enumerated or saved without an explicit administrator action.
    auto *gw = gatewayOf(app);
    QVERIFY(gw != nullptr);
    for (int i = 0; i < 10 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY2(gw->isOnline(), "the simulated gateway must come online at startup");

    QCOMPARE(discovery.requests, 0);

    auto *page = settingsPageOf(app);
    QVERIFY(page != nullptr);

    const SerialConnectionSettings settings = page->serialSettings();
    QCOMPARE(settings.port_name, QStringLiteral("COM1"));
    QCOMPARE(int(settings.station), 1);
    QCOMPARE(int(settings.baud_rate), 9600);
    QCOMPARE(int(settings.stop_bits), 1);
    QVERIFY2(isNeutralParity(settings.parity),
             "a fresh database must yield the documented neutral parity default");
    QCOMPARE(int(settings.timeout_ms), 200);
    QCOMPARE(int(settings.read_retries), 1);

    app.shutdown();
}

// --- OB-3 / OB-4 --------------------------------------------------------------

void SerialSettingsApplicationTest::enumerationIsPassiveAndCorrelatedThroughTheCompositionRoot()
{
    StartedApp started;
    ScriptedSerialPortDiscovery discovery;
    started.cfg.serialPortDiscovery = &discovery;

    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);
    loginAsAdmin(db);
    app.coordinator()->setRole(Role::Admin);

    auto *gw = gatewayOf(app);
    QVERIFY(gw != nullptr);
    for (int i = 0; i < 10 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    auto *page = settingsPageOf(app);
    QVERIFY(page != nullptr);

    const quint64 generationBefore = app.gateway()->gatewayGeneration();

    emit page->enumerateSerialPortsRequested();
    QTRY_COMPARE_WITH_TIMEOUT(discovery.requests, 1, 5000);

    const quint64 requestId = discovery.last_request_id;
    QVERIFY2(requestId != 0, "the enumeration request must carry a non-zero id");

    // While the enumeration is outstanding the active gateway is untouched.
    QCOMPARE(app.gateway()->gatewayGeneration(), generationBefore);
    QVERIFY2(gw->isOnline(), "enumeration must not stop or reconnect the active gateway");

    discovery.completeWith(requestId,
                           {descriptorFor(QStringLiteral("COM1")),
                            descriptorFor(QStringLiteral("COM3"))});

    QTRY_VERIFY_WITH_TIMEOUT(comboHasPort(page->serialPortComboBox(),
                                          QStringLiteral("COM3")), 5000);
    QVERIFY(comboHasPort(page->serialPortComboBox(), QStringLiteral("COM1")));
    QVERIFY2(page->serialPortEdit()->isEnabled(),
             "manual entry must remain available after enumeration");
    QCOMPARE(page->serialSettings().port_name, QStringLiteral("COM1"));

    // Passive: the completion must not have changed, stopped or replaced the
    // active gateway either.
    QCOMPARE(app.gateway()->gatewayGeneration(), generationBefore);
    QVERIFY(gw->isOnline());

    app.shutdown();
}

void SerialSettingsApplicationTest::unmatchedEnumerationCompletionDoesNotAffectThePage()
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

    emit page->enumerateSerialPortsRequested();
    QTRY_COMPARE_WITH_TIMEOUT(discovery.requests, 1, 5000);

    const quint64 outstanding = discovery.last_request_id;
    const int before = page->serialPortComboBox()->count();

    // A completion carrying an unrelated request id arrives first.
    discovery.completeWith(outstanding + 1000, {descriptorFor(QStringLiteral("COM9"))});
    QTest::qWait(100);
    QCOMPARE(page->serialPortComboBox()->count(), before);
    QVERIFY2(!comboHasPort(page->serialPortComboBox(), QStringLiteral("COM9")),
             "a completion for an unknown request id must not affect the page");

    // The matching completion is the one that updates the page.
    discovery.completeWith(outstanding,
                           {descriptorFor(QStringLiteral("COM3")),
                            descriptorFor(QStringLiteral("COM5"))});
    QTRY_VERIFY_WITH_TIMEOUT(page->serialPortComboBox()->count() > before, 5000);
    QVERIFY(comboHasPort(page->serialPortComboBox(), QStringLiteral("COM3")));

    app.shutdown();
}

void SerialSettingsApplicationTest::missingSavedPortIsReportedThroughTheCompositionRootAndNotReplaced()
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

    // Persist a port that the following enumeration will not report.
    SerialConnectionSettings saved;
    saved.port_name = QStringLiteral("COM9");
    emit page->saveSerialSettingsRequested(saved);
    QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 10000);
    QVERIFY(results.last().committed);
    QCOMPARE(page->serialSettings().port_name, QStringLiteral("COM9"));

    const QString before = page->serialSettingsStatusText();

    emit page->enumerateSerialPortsRequested();
    QTRY_COMPARE_WITH_TIMEOUT(discovery.requests, 1, 5000);
    discovery.completeWith(discovery.last_request_id,
                           {descriptorFor(QStringLiteral("COM1")),
                            descriptorFor(QStringLiteral("COM3"))});

    QTRY_VERIFY_WITH_TIMEOUT(page->serialSettingsStatusText() != before, 5000);
    QVERIFY2(!page->serialSettingsStatusText().trimmed().isEmpty(),
             "a missing saved port must be visibly reported");
    QVERIFY2(page->serialSettings().port_name == QStringLiteral("COM9"),
             "the missing saved port must not be silently replaced by a discovered port");

    app.shutdown();
}

// --- OB-5 / OB-6 / OB-7 -------------------------------------------------------

void SerialSettingsApplicationTest::saveCommitsAllSevenKeysWithOneCorrelatedResult()
{
    StartedApp started;
    ScriptedSerialPortDiscovery discovery;
    started.cfg.serialPortDiscovery = &discovery;

    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);
    loginAsAdmin(db);
    app.coordinator()->setRole(Role::Admin);

    auto *gw = gatewayOf(app);
    QVERIFY(gw != nullptr);
    for (int i = 0; i < 10 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    auto *page = settingsPageOf(app);
    QVERIFY(page != nullptr);

    QVector<SettingsBatchResult> results;
    connect(db, &DatabaseService::serialSettingsBatchSaved, this,
            [&results](const SettingsBatchResult &result) { results.append(result); });

    SerialConnectionSettings custom;
    custom.port_name = QStringLiteral("COM7");
    custom.station = 2;
    custom.baud_rate = 19200;
    custom.stop_bits = 2;
    custom.timeout_ms = 500;
    custom.read_retries = 3;

    const quint64 generationBefore = app.gateway()->gatewayGeneration();

    emit page->saveSerialSettingsRequested(custom);
    QVERIFY2(!page->serialSettingsStatusText().trimmed().isEmpty(),
             "the save request must produce an immediate pending status");

    QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 10000);

    // Exactly one correlated batch result for the request.
    QTest::qWait(500);
    QCOMPARE(int(results.size()), 1);
    const SettingsBatchResult result = results[0];
    QVERIFY2(result.committed, qPrintable(QStringLiteral("the save failed: %1").arg(result.error)));
    QVERIFY(result.error.isEmpty());
    QVERIFY2(result.batch_id != 0,
             "the batch result must carry its originating batch id");

    // All keys are persisted and the previous values are gone.
    const QString databasePath = started.databasePath();
    QTRY_VERIFY_WITH_TIMEOUT(valuesContain(databasePath, QStringLiteral("COM7")), 10000);
    QVERIFY(valuesContain(databasePath, QStringLiteral("19200")));
    QVERIFY(valuesContain(databasePath, QStringLiteral("500")));
    QVERIFY2(!valuesContain(databasePath, QStringLiteral("COM1")),
             "the previous port value must not survive a committed batch");
    QVERIFY2(!valuesContain(databasePath, QStringLiteral("9600")),
             "the previous baud value must not survive a committed batch");

    // The page projects the committed settings and a visible terminal result.
    QCOMPARE(page->serialSettings().port_name, QStringLiteral("COM7"));
    QCOMPARE(int(page->serialSettings().baud_rate), 19200);
    QVERIFY2(!page->serialSettingsStatusText().trimmed().isEmpty(),
             "the terminal batch result must be visible");
    QVERIFY2(!page->serialSettingsStatusText().contains(QStringLiteral("失败")),
             "a successful batch must not be presented as a failure");

    // The active gateway is never stopped; a rebuild, if it happens, only
    // follows the successful matching result and never goes backwards.
    QVERIFY2(gw->isOnline(), "a committed serial save must not leave the gateway offline");
    QVERIFY2(app.gateway()->gatewayGeneration() >= generationBefore,
             "the gateway generation must never regress after a committed save");

    app.shutdown();
}

void SerialSettingsApplicationTest::duplicateInFlightSaveIsRejectedAndCannotReplaceTheAcceptedSave()
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

    SerialConnectionSettings first;
    first.port_name = QStringLiteral("COM5");
    SerialConnectionSettings second;
    second.port_name = QStringLiteral("COM6");

    emit page->saveSerialSettingsRequested(first);
    emit page->saveSerialSettingsRequested(second);

    QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 20000);

    // Bounded observation window: no second batch may commit and the persisted
    // port must not switch to the rejected duplicate. Every sample read from the
    // committed database is accumulated so a transient read cannot hide a value.
    const QString databasePath = started.databasePath();
    QElapsedTimer settle;
    settle.start();
    int maxResults = int(results.size());
    QStringList observedValues;
    while (settle.elapsed() < 2500) {
        QTest::qWait(50);
        maxResults = qMax(maxResults, int(results.size()));
        observedValues.append(storedTextValues(databasePath));
        if (maxResults > 1)
            break;
    }
    observedValues.append(storedTextValues(databasePath));

    QVERIFY2(!observedValues.isEmpty(),
             "the probe could not read any persisted value");
    QCOMPARE(maxResults, 1);
    QVERIFY2(results[0].committed,
             qPrintable(QStringLiteral("the accepted save failed: %1").arg(results[0].error)));
    QVERIFY2(observedValues.contains(QStringLiteral("COM5")),
             "the accepted save must be the one that committed");
    QVERIFY2(!observedValues.contains(QStringLiteral("COM6")),
             "an in-flight duplicate save must not replace the accepted save");
    QVERIFY2(!page->serialSettingsStatusText().trimmed().isEmpty(),
             "the duplicate attempt must not return silently");

    // The rejection must not latch the save path: a later explicit save works.
    emit page->saveSerialSettingsRequested(second);
    QTRY_VERIFY_WITH_TIMEOUT(results.size() >= 2, 20000);
    QVERIFY2(results.last().committed,
             qPrintable(QStringLiteral("the later save failed: %1").arg(results.last().error)));
    QTRY_VERIFY_WITH_TIMEOUT(valuesContain(databasePath, QStringLiteral("COM6")), 10000);

    app.shutdown();
}

// --- OB-5 / OB-7 / OB-8 -------------------------------------------------------

void SerialSettingsApplicationTest::failedBatchLeavesCommittedValuesAndGatewayUntouchedAndALaterSaveSucceeds()
{
    StartedApp started;
    ScriptedSerialPortDiscovery discovery;
    started.cfg.serialPortDiscovery = &discovery;

    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);
    loginAsAdmin(db);
    app.coordinator()->setRole(Role::Admin);

    auto *gw = gatewayOf(app);
    QVERIFY(gw != nullptr);
    for (int i = 0; i < 10 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    auto *page = settingsPageOf(app);
    QVERIFY(page != nullptr);

    QVector<SettingsBatchResult> results;
    connect(db, &DatabaseService::serialSettingsBatchSaved, this,
            [&results](const SettingsBatchResult &result) { results.append(result); });

    const QString databasePath = started.databasePath();

    // Precondition: one committed baseline the failed batch must preserve.
    SerialConnectionSettings baseline;
    baseline.port_name = QStringLiteral("COM7");
    baseline.baud_rate = 19200;
    emit page->saveSerialSettingsRequested(baseline);
    QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 10000);
    QVERIFY(results.last().committed);
    const SettingsBatchResult baselineResult = results.last();
    QTRY_VERIFY_WITH_TIMEOUT(valuesContain(databasePath, QStringLiteral("COM7")), 10000);

    const quint64 generationAfterBaseline = app.gateway()->gatewayGeneration();
    const int resultsBeforeFailure = results.size();

    // Inject the database failure by holding the SQLite write lock during the
    // next save only.
    DatabaseWriteLock writeLock(databasePath);
    QVERIFY2(writeLock.acquire(),
             "the probe could not acquire the database write lock");

    SerialConnectionSettings replacement;
    replacement.port_name = QStringLiteral("COM9");
    replacement.baud_rate = 38400;
    emit page->saveSerialSettingsRequested(replacement);

    QTRY_VERIFY_WITH_TIMEOUT(results.size() > resultsBeforeFailure, 30000);
    const SettingsBatchResult failedResult = results.last();
    QVERIFY2(!failedResult.committed,
             "a save under the injected database failure must fail");
    QVERIFY2(!failedResult.error.isEmpty(), "a failed batch must carry an error");
    QVERIFY2(failedResult.batch_id != 0, "the failure result must carry its batch id");
    QVERIFY2(failedResult.batch_id != baselineResult.batch_id,
             "each save request must receive its own batch id");

    // No partial write: every previously committed value is untouched and no
    // replacement value is observable.
    const QStringList lockedValues = storedTextValues(writeLock.connection());
    QVERIFY2(lockedValues.contains(QStringLiteral("COM7")),
             "the previously committed port must survive a failed batch");
    QVERIFY2(lockedValues.contains(QStringLiteral("19200")),
             "the previously committed baud must survive a failed batch");
    QVERIFY2(!lockedValues.contains(QStringLiteral("COM9")),
             "a failed batch must not write any replacement key");
    QVERIFY2(!lockedValues.contains(QStringLiteral("38400")),
             "a failed batch must not write any replacement key");

    // One visible terminal failure; never a success claim.
    const QString failedStatus = page->serialSettingsStatusText();
    QVERIFY2(!failedStatus.trimmed().isEmpty(), "the failed save must be visibly reported");
    QVERIFY2(!failedStatus.contains(QStringLiteral("已保存"))
                 && !failedStatus.contains(QStringLiteral("成功")),
             "a failed save must not claim success");

    // The active gateway and the persisted settings remain as they were.
    QCOMPARE(app.gateway()->gatewayGeneration(), generationAfterBaseline);
    QVERIFY2(gw->isOnline(), "a failed batch must not stop or replace the active gateway");

    // A later valid save can still succeed (OB-8).
    writeLock.release();
    emit page->saveSerialSettingsRequested(replacement);
    QTRY_VERIFY_WITH_TIMEOUT(results.size() > resultsBeforeFailure + 1, 30000);
    QVERIFY2(results.last().committed,
             qPrintable(QStringLiteral("the retry failed: %1").arg(results.last().error)));
    QTRY_VERIFY_WITH_TIMEOUT(valuesContain(databasePath, QStringLiteral("COM9")), 10000);
    QVERIFY2(!valuesContain(databasePath, QStringLiteral("COM7")),
             "the successful retry must replace the previously committed port");

    app.shutdown();
}

// --- OB-5 / OB-10: all-seven-key persistence and preservation -----------------

void SerialSettingsApplicationTest::committedBatchEchoesAllSevenKeysAfterRestart()
{
    StartedApp started;

    SerialConnectionSettings custom;
    custom.port_name = QStringLiteral("COM7");
    custom.station = 3;
    custom.baud_rate = 19200;
    custom.stop_bits = 2;
    setAlternateParity(custom.parity);
    custom.timeout_ms = 500;
    custom.read_retries = 3;

    {
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

        emit page->saveSerialSettingsRequested(custom);
        QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 10000);
        QVERIFY2(results.last().committed,
                 qPrintable(QStringLiteral("the save failed: %1").arg(results.last().error)));

        app.shutdown();
    }

    {
        ScriptedSerialPortDiscovery discovery;
        started.cfg.serialPortDiscovery = &discovery;

        Application app(started.cfg);
        DatabaseService *db = nullptr;
        startAndWaitReady(app, db);
        loginAsAdmin(db, /*adminAlreadyCreated=*/true);

        auto *page = settingsPageOf(app);
        QVERIFY(page != nullptr);

        // Every one of the seven committed keys must be echoed after restart,
        // not only the port and baud.
        const SerialConnectionSettings echoed = page->serialSettings();
        QCOMPARE(echoed.port_name, custom.port_name);
        QCOMPARE(int(echoed.station), int(custom.station));
        QCOMPARE(int(echoed.baud_rate), int(custom.baud_rate));
        QCOMPARE(int(echoed.stop_bits), int(custom.stop_bits));
        QVERIFY2(!isNeutralParity(echoed.parity),
                 "a persisted non-neutral parity must survive a restart");
        QCOMPARE(int(echoed.timeout_ms), int(custom.timeout_ms));
        QCOMPARE(int(echoed.read_retries), int(custom.read_retries));
        QCOMPARE(discovery.requests, 0);

        app.shutdown();
    }
}

void SerialSettingsApplicationTest::failedBatchLeavesAllSevenKeysUnchangedAcrossRestart()
{
    StartedApp started;

    SerialConnectionSettings baseline;
    baseline.port_name = QStringLiteral("COM7");
    baseline.station = 3;
    baseline.baud_rate = 19200;
    baseline.stop_bits = 2;
    setAlternateParity(baseline.parity);
    baseline.timeout_ms = 500;
    baseline.read_retries = 3;

    {
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

        emit page->saveSerialSettingsRequested(baseline);
        QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 10000);
        QVERIFY(results.last().committed);
        const int resultsBeforeFailure = results.size();

        // Inject the database failure by holding the SQLite write lock during
        // the replacement save only; all seven replacement values differ from
        // the committed baseline.
        DatabaseWriteLock writeLock(started.databasePath());
        QVERIFY2(writeLock.acquire(),
                 "the probe could not acquire the database write lock");

        SerialConnectionSettings replacement;
        replacement.port_name = QStringLiteral("COM9");
        replacement.station = 4;
        replacement.baud_rate = 38400;
        replacement.stop_bits = 1;
        replacement.timeout_ms = 700;
        replacement.read_retries = 5;

        emit page->saveSerialSettingsRequested(replacement);
        QTRY_VERIFY_WITH_TIMEOUT(results.size() > resultsBeforeFailure, 30000);
        QVERIFY2(!results.last().committed,
                 "a save under the injected database failure must fail");
        QVERIFY2(!results.last().error.isEmpty(), "a failed batch must carry an error");

        writeLock.release();
        app.shutdown();
    }

    {
        ScriptedSerialPortDiscovery discovery;
        started.cfg.serialPortDiscovery = &discovery;

        Application app(started.cfg);
        DatabaseService *db = nullptr;
        startAndWaitReady(app, db);
        loginAsAdmin(db, /*adminAlreadyCreated=*/true);

        auto *page = settingsPageOf(app);
        QVERIFY(page != nullptr);

        // None of the seven keys may have been overwritten by the failed batch.
        const SerialConnectionSettings echoed = page->serialSettings();
        QCOMPARE(echoed.port_name, baseline.port_name);
        QCOMPARE(int(echoed.station), int(baseline.station));
        QCOMPARE(int(echoed.baud_rate), int(baseline.baud_rate));
        QCOMPARE(int(echoed.stop_bits), int(baseline.stop_bits));
        QVERIFY2(!isNeutralParity(echoed.parity),
                 "the committed non-neutral parity must survive a failed replacement batch");
        QCOMPARE(int(echoed.timeout_ms), int(baseline.timeout_ms));
        QCOMPARE(int(echoed.read_retries), int(baseline.read_retries));

        app.shutdown();
    }
}

// --- OB-10 --------------------------------------------------------------------

void SerialSettingsApplicationTest::persistedSettingsAreEchoedAfterRestart()
{
    StartedApp started;

    {
        ScriptedSerialPortDiscovery discovery;
        started.cfg.serialPortDiscovery = &discovery;

        Application app(started.cfg);
        DatabaseService *db = nullptr;
        startAndWaitReady(app, db);
        // First-run flow: the helper creates the initial administrator first.
        loginAsAdmin(db);
        app.coordinator()->setRole(Role::Admin);

        auto *page = settingsPageOf(app);
        QVERIFY(page != nullptr);

        QVector<SettingsBatchResult> results;
        connect(db, &DatabaseService::serialSettingsBatchSaved, this,
                [&results](const SettingsBatchResult &result) { results.append(result); });

        SerialConnectionSettings custom;
        custom.port_name = QStringLiteral("COM7");
        custom.station = 2;
        custom.baud_rate = 19200;
        emit page->saveSerialSettingsRequested(custom);
        QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), 10000);
        QVERIFY(results.last().committed);

        app.shutdown();
    }

    {
        ScriptedSerialPortDiscovery discovery;
        started.cfg.serialPortDiscovery = &discovery;

        Application app(started.cfg);
        DatabaseService *db = nullptr;
        startAndWaitReady(app, db);
        // The administrator was explicitly created and committed in the first
        // session above; only the login is repeated on this restart.
        loginAsAdmin(db, /*adminAlreadyCreated=*/true);

        auto *page = settingsPageOf(app);
        QVERIFY(page != nullptr);

        const SerialConnectionSettings echoed = page->serialSettings();
        QCOMPARE(echoed.port_name, QStringLiteral("COM7"));
        QCOMPARE(int(echoed.station), 2);
        QCOMPARE(int(echoed.baud_rate), 19200);
        QCOMPARE(discovery.requests, 0);

        app.shutdown();
    }
}

QTEST_MAIN(SerialSettingsApplicationTest)
#include "serial_settings_application_test.moc"
