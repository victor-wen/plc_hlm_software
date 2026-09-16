// PLC-HMI-003 developer regression tests (D9): correlated submission
// completions, stale/obsolete identity rejection, serial-save/gateway
// isolation, defensive parameter-write timeout, real block quality/age and
// independent D210 validity, and M112 absence. These complement (never
// replace) the independent black-box tests in
// tests/unit/plc_*_test.cpp and tests/integration/plc_m112_d140_session_test.cpp.

#include <QtTest>

#include <QTemporaryDir>

#include <memory>

#include "adapters/modbus/modbus_transport.h"
#include "adapters/modbus/qt_modbus_plc_gateway.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/lifecycle_controller.h"
#include "app/configuration.h"
#include "application/control_coordinator.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"
#include "ui/MainWindow.h"
#include "ui/pages/users_settings_page.h"

using namespace hlm;

namespace {

constexpr quint16 kM101 = 101;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kM112 = 112;
constexpr quint16 kD128 = 128;
constexpr quint16 kD130 = 130;
constexpr quint16 kD210 = 210;

constexpr quint64 kAdjustTimeoutMs = 3'600'001;

void homeReady(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

void putInAutoMode(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM104, true);
    gw.tick();
}

// Fake gateway-facing submission source with full control over request
// identity and generation (same pattern as the independent correlation test).
class FakeSubmissionSource
{
public:
    ControlCoordinator::PulseTransport transport()
    {
        ControlCoordinator::PulseTransport t;
        t.startPulse = [this](quint16 address) -> SubmissionResult {
            return record(PlcOperation::Pulse, address, false, 0);
        };
        t.writeHold = [this](quint16 address, bool value) -> SubmissionResult {
            return record(PlcOperation::WriteCoil, address, value, 0);
        };
        t.writeCoil = [this](quint16 address, bool value, CommandPriority)
            -> SubmissionResult {
            return record(PlcOperation::WriteCoil, address, value, 0);
        };
        t.writeRegister = [this](quint16 address, quint16 value, CommandPriority)
            -> SubmissionResult {
            return record(PlcOperation::WriteRegister, address, false, value);
        };
        return t;
    }

    quint64 generation = 1;
    quint64 lastRequestId = 0;
    quint16 lastAddress = 0;
    PlcOperation lastOperation = PlcOperation::WriteCoil;

    void complete(ControlCoordinator &coordinator, quint64 requestId, bool result,
                  quint64 completionGeneration, const QString &error = QString())
    {
        SubmissionCompletion completion;
        completion.request_id = requestId;
        completion.gateway_generation = completionGeneration;
        completion.operation = lastOperation;
        completion.address = lastAddress;
        completion.result = result;
        completion.error = error;
        coordinator.onSubmissionCompleted(completion);
    }

private:
    SubmissionResult record(PlcOperation operation, quint16 address, bool coilValue,
                            quint16 registerValue)
    {
        lastOperation = operation;
        lastAddress = address;
        SubmissionResult r;
        r.accepted = true;
        r.request_id = ++m_nextRequestId;
        r.gateway_generation = generation;
        lastRequestId = r.request_id;
        Q_UNUSED(coilValue);
        Q_UNUSED(registerValue);
        return r;
    }

    quint64 m_nextRequestId = 0;
};

// A deterministic IModbusTransport for the adapter defensive-timeout test.
class SilentTransport : public IModbusTransport
{
    Q_OBJECT
public:
    bool open() override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }
    bool send(const ModbusRequest &req) override
    {
        sent.append(req);
        return true;
    }

    void completeOk(const QList<quint16> &values)
    {
        TransferResult res;
        res.ok = true;
        res.values = values;
        emit transferFinished(res);
    }

    QList<ModbusRequest> sent;
};

// Deterministic worker + silent transport + injected clock.
struct SimpleWorkerRig
{
    SimpleWorkerRig()
        : worker(QtModbusPlcGateway::Config(), &transport)
    {
        worker.setNowMs([this]() { return now; });
        worker.setPollIntervals(250, 1000000, 1000000, 1000000);
        worker.setWriteConfirmTimeoutMs(5000);
        QObject::connect(&worker, &ModbusGatewayWorker::submissionCompleted, &worker,
                         [this](const SubmissionCompletion &c) {
                             completions.append(c);
                         });
        worker.start();
    }

    SilentTransport transport;
    ModbusGatewayWorker worker;
    qint64 now = 0;
    QVector<SubmissionCompletion> completions;
};

// Application fixture on the simulated gateway.
struct SimApp
{
    QTemporaryDir dir;
    std::unique_ptr<Application> app;

    explicit SimApp()
    {
        auto *cfg = new AppConfig;
        cfg->useSimulatedGateway = true;
        cfg->simulatedTickIntervalMs = 0;
        cfg->databasePath = dir.filePath(QStringLiteral("app.db"));
        app = std::make_unique<Application>(*cfg);
        delete cfg;
        app->start();
    }
};

} // namespace

class PlcCorrelationDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    // --- port: exactly-one correlated completion ------------------------------
    void everyAcceptedSubmissionCompletesExactlyOnce();
    void rejectedSubmissionNeverCompletes();

    // --- coordinator: stale / obsolete identity rejection ---------------------
    void unknownAndObsoleteCompletionsAreIgnored();

    // --- application: serial save keeps the simulator; convergence unchanged --
    void serialSaveKeepsTheSimulatorAndConvergenceStillApplies();

    // --- adapter: defensive parameter-write confirmation timeout --------------
    void parameterWriteConfirmationTimeoutConverges();

    // --- snapshot: real quality/age and independent D210 validity -------------
    void realBlockQualityAndAges();
    void widthDeltaValidityIsIndependentAndRangeBounded();

    // --- M112 removal ---------------------------------------------------------
    void onlineSessionNeverSubmitsCoil112();
};

// --- port ---------------------------------------------------------------------

void PlcCorrelationDeveloperTest::everyAcceptedSubmissionCompletesExactlyOnce()
{
    SimulatedPlcGateway gw;
    gw.start();
    QVERIFY(gw.isOnline());

    QVector<SubmissionCompletion> completions;
    connect(&gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&completions](const SubmissionCompletion &c) { completions.append(c); });

    const SubmissionResult coil = gw.submitWriteCoil(kM106, true);
    const SubmissionResult reg = gw.submitWriteRegister(kD128, 300);
    const SubmissionResult pulse = gw.submitPulse(kM101);
    QVERIFY(coil.accepted && reg.accepted && pulse.accepted);
    QVERIFY(coil.request_id != 0 && reg.request_id != 0 && pulse.request_id != 0);
    QCOMPARE(coil.gateway_generation, gw.gatewayGeneration());

    gw.tick();
    QCOMPARE(completions.size(), 3);
    for (const SubmissionResult &submitted : {coil, reg, pulse}) {
        int matches = 0;
        for (const SubmissionCompletion &c : completions) {
            if (c.request_id != submitted.request_id)
                continue;
            ++matches;
            QCOMPARE(c.gateway_generation, submitted.gateway_generation);
        }
        QCOMPARE(matches, 1); // exactly one terminal completion
    }

    // Further ticks must not repeat completions.
    gw.tick();
    QCOMPARE(completions.size(), 3);
}

void PlcCorrelationDeveloperTest::rejectedSubmissionNeverCompletes()
{
    SimulatedPlcGateway gw;
    gw.start();
    gw.setLinkDown(true);
    QVERIFY(!gw.isOnline());

    QVector<SubmissionCompletion> completions;
    connect(&gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&completions](const SubmissionCompletion &c) { completions.append(c); });

    const SubmissionResult rejected = gw.submitWriteCoil(kM106, true);
    QVERIFY(!rejected.accepted);
    QCOMPARE(rejected.request_id, quint64(0));
    QVERIFY(!rejected.immediate_rejection_reason.isEmpty());
    gw.tick();
    QVERIFY(completions.isEmpty());
}

// --- coordinator --------------------------------------------------------------

void PlcCorrelationDeveloperTest::unknownAndObsoleteCompletionsAreIgnored()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    FakeSubmissionSource fake;
    // Accept (but never auto-apply) each submission so the command stays
    // pending until a correlated completion arrives.
    ControlCoordinator coordinator(fake.transport(), ControlCoordinator::Config(),
                                   [&now]() { return now; });
    connect(&gw, &SimulatedPlcGateway::snapshotReady, &coordinator,
            [&coordinator](quint64, const DeviceSnapshot &s) {
                coordinator.onSnapshot(s);
            });
    connect(&gw, &SimulatedPlcGateway::connectionStateChanged, &coordinator,
            [&coordinator](quint64, bool online) {
                coordinator.onConnectionChanged(online);
            });
    if (gw.hasSnapshot())
        coordinator.onSnapshot(gw.lastSnapshot());
    coordinator.setRole(Role::Admin);
    homeReady(gw);
    putInAutoMode(gw);

    QVector<bool> outcomes;
    connect(&coordinator, &ControlCoordinator::commandResult, this,
            [&outcomes](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Reset)
                    outcomes.append(ok);
            });

    QVERIFY(coordinator.reset().accepted);
    const quint64 requestId = fake.lastRequestId;
    QVERIFY(requestId != 0);

    // Unknown request id and an obsolete/future generation are both ignored.
    fake.complete(coordinator, requestId + 424242, false, fake.generation);
    fake.complete(coordinator, requestId, false, fake.generation + 7);
    fake.complete(coordinator, requestId, false, 0);
    QVERIFY(outcomes.isEmpty());
    QVERIFY(coordinator.resetInProgress());

    // The matching identity converges exactly once; duplicates are ignored.
    fake.complete(coordinator, requestId, false, fake.generation);
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);
    fake.complete(coordinator, requestId, true, fake.generation);
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!coordinator.resetInProgress());
}

// --- application: replacement isolation ---------------------------------------

void PlcCorrelationDeveloperTest::serialSaveKeepsTheSimulatorAndConvergenceStillApplies()
{
    SimApp fixture;
    Application *app = fixture.app.get();
    auto *gw1 = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    QVERIFY(gw1 != nullptr);
    for (int i = 0; i < 10 && !gw1->isOnline(); ++i)
        gw1->tick();
    QVERIFY(gw1->isOnline());
    const quint64 generationBefore = gw1->gatewayGeneration();
    QVERIFY(generationBefore != 0);

    // A pending command exists while the serial settings are saved.
    app->coordinator()->setRole(Role::Admin);
    homeReady(*gw1);
    putInAutoMode(*gw1);
    QVERIFY(app->coordinator()->reset().accepted);
    QVERIFY(app->coordinator()->resetInProgress());

    // Stage a session username so the settings batch carries updated_by.
    UserRecord admin;
    admin.id = 1;
    admin.username = QStringLiteral("admin");
    admin.role = Role::Admin;
    app->lifecycle()->onLoginSucceeded(admin);

    auto *usersPage = app->window()->findChild<UsersSettingsPage *>();
    QVERIFY(usersPage != nullptr);
    SerialConnectionSettings cfg = AppConfig().serial;
    cfg.port_name = QStringLiteral("SIM");
    QSignalSpy batchSpy(app->database(),
                        &DatabaseService::serialSettingsBatchSaved);
    emit usersPage->saveSerialSettingsRequested(cfg);
    QTRY_VERIFY_WITH_TIMEOUT(batchSpy.count() >= 1, 10000);

    // PLC-HMI-004 D6 supersedes the old "serial save -> rebuild" trigger:
    // serial transport settings only affect the real Modbus gateway, while the
    // in-process simulator has no serial transport. A committed save must not
    // stop, replace or regress the simulator, must report exactly one
    // correlated batch result, and must not disturb an in-flight command.
    QCOMPARE(batchSpy.count(), 1);
    const SettingsBatchResult result =
        batchSpy[0][0].value<SettingsBatchResult>();
    QVERIFY2(result.committed, qPrintable(result.error));
    QVERIFY(result.batch_id != 0);
    QVERIFY(app->gateway() == static_cast<IPlcGateway *>(gw1));
    QCOMPARE(app->gateway()->gatewayGeneration(), generationBefore);
    QVERIFY(gw1->isOnline());
    QVERIFY2(app->coordinator()->resetInProgress(),
             "a serial save must not silently converge or drop a pending command");

    // A late completion from an older generation must not affect the pending
    // command: the composition root rejects obsolete generations.
    QVector<bool> outcomes;
    connect(app->coordinator(), &ControlCoordinator::commandResult, this,
            [&outcomes](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Reset)
                    outcomes.append(ok);
            });
    SubmissionCompletion stale;
    stale.request_id = 999999;
    stale.gateway_generation = generationBefore;
    stale.operation = PlcOperation::WriteCoil;
    stale.address = kM104;
    stale.result = false;
    app->coordinator()->onSubmissionCompleted(stale);
    QVERIFY(outcomes.isEmpty());

    // The replacement/offline convergence machinery itself is unchanged: the
    // same coordinator call the real rebuild path performs converges the
    // pending command exactly once, with a visible failure.
    app->coordinator()->onConnectionChanged(false);
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);
    QVERIFY2(!app->coordinator()->resetInProgress(),
             "the pending command must converge when the gateway goes offline");

    app->shutdown();
}

// --- adapter: defensive parameter-write timeout -------------------------------

void PlcCorrelationDeveloperTest::parameterWriteConfirmationTimeoutConverges()
{
    SimpleWorkerRig rig;
    // Complete the immediate first fast poll so the worker is online and idle.
    rig.transport.completeOk(QList<quint16>(41, 0));
    QVERIFY(rig.worker.isOnline());

    // Parameter write accepted; the write is acknowledged but its readback is
    // never delivered (lost confirmation). The defensive timeout converges it.
    const SubmissionResult submitted =
        rig.worker.submitWriteRegister(kD128, 300, CommandPriority::Normal);
    QVERIFY(submitted.accepted);
    QCOMPARE(rig.transport.sent.last().kind, ModbusRequest::Kind::WriteRegister);

    // Acknowledge the write; the readback is queued but never answered.
    rig.transport.completeOk({});
    QVERIFY(rig.completions.isEmpty());

    // Advance past the defensive confirmation timeout and drive the tick.
    rig.now = 6000;
    rig.worker.onPollTick();
    QCOMPARE(rig.completions.size(), 1);
    QCOMPARE(rig.completions.first().request_id, submitted.request_id);
    QCOMPARE(rig.completions.first().gateway_generation, submitted.gateway_generation);
    QCOMPARE(rig.completions.first().operation, PlcOperation::WriteRegister);
    QVERIFY(!rig.completions.first().result);
    QVERIFY(!rig.completions.first().error.isEmpty());

    // A further tick must not repeat the completion.
    rig.now = 12000;
    rig.worker.onPollTick();
    QCOMPARE(rig.completions.size(), 1);
}

// --- snapshot quality/age and D210 --------------------------------------------

void PlcCorrelationDeveloperTest::realBlockQualityAndAges()
{
    SimulatedPlcGateway gw;
    gw.start();
    for (int i = 0; i < 3; ++i)
        gw.tick();
    const DeviceSnapshot s = gw.lastSnapshot();

    QVERIFY(s.fast_quality == DataQuality::Valid);
    QVERIFY(s.home_quality == DataQuality::Valid);
    QVERIFY(s.command_quality == DataQuality::Valid);
    QVERIFY(s.slow_quality == DataQuality::Valid);
    QVERIFY(s.overall_quality == DataQuality::Valid);
    QVERIFY(s.fast_age_ms >= 0 && s.home_age_ms >= 0);
    QVERIFY(s.command_age_ms >= 0 && s.slow_age_ms >= 0);
    QCOMPARE(s.overall_age_ms,
             std::max({s.fast_age_ms, s.home_age_ms, s.command_age_ms, s.slow_age_ms}));

    // A failed fast transfer is never presented as valid and a successful
    // refresh clears it.
    gw.setLinkDown(true);
    gw.tick();
    QVERIFY(!gw.isOnline());
    gw.setLinkDown(false);
    gw.tick();
    QVERIFY(gw.isOnline());
    gw.tick();
    QVERIFY(gw.lastSnapshot().fast_quality == DataQuality::Valid);
}

void PlcCorrelationDeveloperTest::widthDeltaValidityIsIndependentAndRangeBounded()
{
    SimulatedPlcGateway gw;
    gw.start();
    gw.tick();

    struct DeltaCase
    {
        quint16 target;
        quint16 current;
        qint16 delta;
        bool expected;
    };
    const QVector<DeltaCase> cases{
        {200, 150, 50, true},
        {400, 50, 350, true},
        {50, 400, -350, true},
        {401, 50, 351, false},
        {50, 401, -351, false},
    };
    for (const DeltaCase &c : cases) {
        gw.model().writeRegister(kD128, c.target);
        gw.model().writeRegister(kD130, c.current);
        gw.model().writeRegister(kD210, static_cast<quint16>(c.delta));
        gw.tick();
        QCOMPARE(gw.lastSnapshot().width_delta_valid, c.expected);
    }

    // In-range D210 with an out-of-range D130 must stay independently valid.
    gw.model().writeRegister(kD128, 200);
    gw.model().writeRegister(kD130, 0);
    gw.model().writeRegister(kD210, 200);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().fieldValid(SnapshotField::CurrentWidth));
    QVERIFY(gw.lastSnapshot().width_delta_valid);
}

// --- M112 absence ---------------------------------------------------------------

void PlcCorrelationDeveloperTest::onlineSessionNeverSubmitsCoil112()
{
    SimApp fixture;
    Application *app = fixture.app.get();
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    QVERIFY(gw != nullptr);
    for (int i = 0; i < 10 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    QVector<quint16> submittedAddresses;
    connect(gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&submittedAddresses](const SubmissionCompletion &c) {
                submittedAddresses.append(c.address);
            });

    app->coordinator()->setRole(Role::Admin);
    homeReady(*gw);
    putInAutoMode(*gw);
    QVERIFY(app->coordinator()->reset().accepted);
    for (int i = 0; i < 40; ++i) {
        gw->tick();
        QVERIFY2(!gw->model().readCoil(kM112),
                 "coil 112 (M112) must never be energized by the HMI");
    }
    for (quint16 address : submittedAddresses)
        QVERIFY2(address != kM112, "the HMI submitted a write to coil 112");

    app->shutdown();
}

QTEST_MAIN(PlcCorrelationDeveloperTest)
#include "plc_correlation_developer_test.moc"
