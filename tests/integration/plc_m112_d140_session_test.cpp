// PLC-HMI-003 black-box integration tests: M112 is gone from the live path,
// D140 changes remain the observable liveness signal, and the composition root
// exposes the revised submission port (brief OB-7, OB-12).
//
// Authored from the behavior-only brief .ai/test-briefs/PLC-HMI-003.yaml and
// the approved .ai/project-contract.yaml. No production implementation source
// was read.
//
// Coverage note: the brief also asks for a *stalled* D140 to produce
// freeze/offline evidence. The simulator gateway's public heartbeat-freeze
// fault hook (the same fault-injection surface already exercised by other
// inspectable tests under tests/) drives that condition, so the case is
// implemented below: freeze D140 while the rest of the machine data stays
// healthy, observe the offline/freeze evidence, then verify recovery when the
// heartbeat resumes. A second coverage addition injects a failure for one poll
// block's transfer only and asserts that block becomes ProtocolError while the
// other blocks stay Valid until a successful refresh.

#include <QtTest>

#include <QTemporaryDir>

#include <memory>

#include "adapters/modbus/modbus_transport.h"
#include "adapters/modbus/qt_modbus_plc_gateway.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"

using namespace hlm;

namespace {

constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kM112 = 112;
constexpr quint16 kD140 = 140;

Application *startSimulatedApplication(const QTemporaryDir &dir,
                                       SimulatedPlcGateway **gwOut)
{
    auto *cfg = new AppConfig;
    cfg->useSimulatedGateway = true;
    cfg->simulatedTickIntervalMs = 0;
    cfg->databasePath = dir.filePath(QStringLiteral("app.db"));

    auto *app = new Application(*cfg);
    delete cfg;

    app->start();
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    if (gwOut != nullptr)
        *gwOut = gw;
    return app;
}

void advanceUntilOnline(SimulatedPlcGateway &gw, int maxTicks = 10)
{
    for (int i = 0; i < maxTicks && !gw.isOnline(); ++i)
        gw.tick();
}

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

// A deterministic transport that never answers on its own: transfers are
// completed explicitly so the exact age of every block's last successful
// transfer is controlled by the injected clock.
class ProbeTransport : public IModbusTransport
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

    // Healthy-session fidelity: a live H3U link keeps D140 changing (the
    // one-second heartbeat is the only liveness evidence, contract F-09 /
    // OB-7). Every successful transfer therefore answers with a response image
    // carrying the current live heartbeat and advances it, wherever the
    // centralized register map places D140 inside a poll block. A block that
    // stops being polled consequently stops refreshing D140, which is exactly
    // the frozen-heartbeat condition the online path must survive first.
    void completeOk()
    {
        TransferResult res;
        res.ok = true;
        res.values = QList<quint16>(41, heartbeatValue);
        ++heartbeatValue;
        emit transferFinished(res);
    }

    // Complete the outstanding transfer as failed (no response image). The
    // caller chooses which request to fail by inspecting sent[completed], so a
    // single poll block can be driven to ProtocolError while the others keep
    // succeeding (contract quality rule for a failed transfer).
    void completeFail()
    {
        TransferResult res;
        res.ok = false;
        emit transferFinished(res);
    }

    QList<ModbusRequest> sent;
    quint16 heartbeatValue = 0;
};

// Poll worker on an injected clock: the block under test keeps a poll interval
// above its stale threshold while the other blocks keep refreshing, so the
// published quality is the only observable that changes.
struct StaleRig
{
    StaleRig(int fastMs, int homeMs, int commandMs, int slowMs)
        : worker(QtModbusPlcGateway::Config(), &transport)
    {
        worker.setNowMs([this]() { return now; });
        worker.setPollIntervals(fastMs, homeMs, commandMs, slowMs);
        QObject::connect(&worker, &ModbusGatewayWorker::snapshotReady, &worker,
                         [this](quint64, const DeviceSnapshot &s) {
                             snapshots.append(s);
                         });
        worker.start();

        // Complete every transfer the worker issues until it is online.
        for (int i = 0; i < 60 && !worker.isOnline(); ++i) {
            if (transport.sent.size() > completed)
                transport.completeOk();
            completed = transport.sent.size();
            worker.onPollTick();
            now += 50;
        }
    }

    ProbeTransport transport;
    ModbusGatewayWorker worker;
    qint64 now = 0;
    int completed = 0;
    QVector<DeviceSnapshot> snapshots;
};

} // namespace

class PlcM112D140SessionTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-7: no M112 write, no M112 liveness --------------------------------
    void onlineSessionNeverSubmitsOrEnergizesCoil112();

    // --- OB-12: revised port end-to-end ---------------------------------------
    void applicationGatewayExposesTheRevisedSubmissionPort();

    // --- OB-7: D140 changes remain the observable liveness signal -------------
    void freshHeartbeatKeepsTheSessionOnlineWithValidData();

    // --- OB-6: comm statistics through the base port, no concrete cast ---------
    void commStatsArriveThroughTheBasePortWithoutConcreteCast();

    // --- OB-8: exact stale thresholds per block -------------------------------
    void staleThresholdsFollowTransferSuccessAge();

    // --- OB-7: a stalled D140 still triggers the offline/freeze path ----------
    void stalledD140TriggersTheOfflineFreezePath();

    // --- OB-8: a failed transfer marks only its source block ProtocolError ----
    void failedTransferMarksOnlyItsBlockProtocolErrorUntilRefresh();
};

// --- OB-7 ---------------------------------------------------------------------

void PlcM112D140SessionTest::onlineSessionNeverSubmitsOrEnergizesCoil112()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
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

    for (int i = 0; i < 60; ++i) {
        gw->tick();
        QVERIFY2(!gw->model().readCoil(kM112),
                 "coil 112 (M112) must never be energized by the HMI");
    }

    // Also exercise the operator-safe commands available to anonymous users.
    app->coordinator()->stop();
    app->coordinator()->estopSet();
    for (int i = 0; i < 20; ++i) {
        gw->tick();
        QVERIFY2(!gw->model().readCoil(kM112),
                 "coil 112 (M112) must never be energized by the HMI");
    }

    for (quint16 address : submittedAddresses) {
        QVERIFY2(address != kM112,
                 "the HMI submitted a write to coil 112 during an online session");
    }

    app->shutdown();
}

void PlcM112D140SessionTest::applicationGatewayExposesTheRevisedSubmissionPort()
{
    // OB-12: the composition root's gateway is used through the revised port
    // (submission identity + correlated completion) without a concrete cast.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);

    IPlcGateway *port = app->gateway();
    QVERIFY(port != nullptr);

    const SubmissionResult submitted = port->submitWriteCoil(kM106, true);
    QVERIFY2(submitted.accepted, "an online submission through the port must be accepted");
    QVERIFY(submitted.request_id != 0);
    QVERIFY(submitted.gateway_generation != 0);

    QVector<quint64> completedIds;
    connect(port, &IPlcGateway::submissionCompleted, this,
            [&completedIds](const SubmissionCompletion &c) {
                completedIds.append(c.request_id);
            });
    for (int i = 0; i < 6; ++i)
        gw->tick();

    QVERIFY2(completedIds.contains(submitted.request_id),
             "the port must report a completion for the accepted request");

    app->shutdown();
}

void PlcM112D140SessionTest::freshHeartbeatKeepsTheSessionOnlineWithValidData()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY(gw->isOnline());

    const quint16 heartbeatBefore = gw->model().readRegister(kD140);
    for (int i = 0; i < 6; ++i)
        gw->tick();
    const quint16 heartbeatAfter = gw->model().readRegister(kD140);

    QVERIFY2(heartbeatAfter != heartbeatBefore,
             "precondition: D140 must change while the session is healthy");
    QVERIFY2(gw->isOnline(), "a changing D140 must keep the session online");
    QVERIFY2(gw->lastSnapshot().overall_quality == DataQuality::Valid,
             "healthy polls must keep the published data valid");

    app->shutdown();
}

// --- OB-6 ---------------------------------------------------------------------

void PlcM112D140SessionTest::commStatsArriveThroughTheBasePortWithoutConcreteCast()
{
    // Contract: communication statistics are emitted by the port itself and the
    // composition root consumes them without qobject_cast'ing a concrete
    // gateway. Connecting through IPlcGateway* proves the base-port contract.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);

    IPlcGateway *port = app->gateway();
    QVERIFY(port != nullptr);

    QVector<PlcCommStats> stats;
    connect(port, &IPlcGateway::commStatsChanged, this,
            [&stats](const PlcCommStats &s) { stats.append(s); });

    for (int i = 0; i < 6; ++i)
        gw->tick();

    QVERIFY2(!stats.isEmpty(),
             "the port must publish communication statistics without a concrete cast");

    const quint64 generation = port->gatewayGeneration();
    for (const PlcCommStats &s : stats) {
        QCOMPARE(s.gateway_generation, generation);
        QVERIFY2(s.per_block_age_ms >= 0,
                 "communication statistics must carry a real, non-negative data age");
    }

    app->shutdown();
}

// --- OB-8 ---------------------------------------------------------------------

void PlcM112D140SessionTest::staleThresholdsFollowTransferSuccessAge()
{
    // Approved thresholds: fast/home 1000 ms, command 2000 ms, slow 4000 ms.
    // The block under test keeps a poll interval far above its threshold so its
    // last success stays at startup while the other blocks keep refreshing;
    // the published age of the block under test is the controlled variable.
    struct ThresholdCase
    {
        int blockIndex; // 0 fast, 1 home, 2 command, 3 slow
        qint64 thresholdMs;
        const char *block;
    };
    const QVector<ThresholdCase> cases{
        {0, 1000, "fast"},
        {1, 1000, "home"},
        {2, 2000, "command"},
        {3, 4000, "slow"},
    };

    for (const ThresholdCase &c : cases) {
        int fastMs = 250;
        int homeMs = 250;
        int commandMs = 250;
        int slowMs = 250;
        switch (c.blockIndex) {
        case 0:
            fastMs = 1000000;
            break;
        case 1:
            homeMs = 1000000;
            break;
        case 2:
            commandMs = 1000000;
            break;
        default:
            slowMs = 1000000;
            break;
        }

        StaleRig rig(fastMs, homeMs, commandMs, slowMs);
        QVERIFY2(rig.worker.isOnline(), c.block);
        QVERIFY2(!rig.snapshots.isEmpty(), "the gateway must publish snapshots");

        auto qualityOf = [&c](const DeviceSnapshot &s) -> DataQuality {
            switch (c.blockIndex) {
            case 0:
                return s.fast_quality;
            case 1:
                return s.home_quality;
            case 2:
                return s.command_quality;
            default:
                return s.slow_quality;
            }
        };
        auto ageOf = [&c](const DeviceSnapshot &s) -> qint64 {
            switch (c.blockIndex) {
            case 0:
                return s.fast_age_ms;
            case 1:
                return s.home_age_ms;
            case 2:
                return s.command_age_ms;
            default:
                return s.slow_age_ms;
            }
        };

        bool sawYoungAndNotStale = false;
        bool sawOldAndStale = false;
        for (int step = 0; step < 800 && !sawOldAndStale; ++step) {
            rig.now += 50;
            rig.worker.onPollTick();
            while (rig.transport.sent.size() > rig.completed) {
                rig.transport.completeOk();
                ++rig.completed;
            }
            if (rig.snapshots.isEmpty())
                continue;

            const DeviceSnapshot &last = rig.snapshots.last();
            const qint64 age = ageOf(last);
            if (age < c.thresholdMs) {
                QVERIFY2(qualityOf(last) != DataQuality::Stale,
                         "a block younger than its stale threshold must not be stale");
                sawYoungAndNotStale = true;
            } else if (age > c.thresholdMs) {
                QVERIFY2(qualityOf(last) == DataQuality::Stale,
                         "a block without a successful transfer within its threshold "
                         "must be Stale even while other blocks keep succeeding");
                sawOldAndStale = true;
            }
        }

        QVERIFY2(sawYoungAndNotStale,
                 "no published snapshot was younger than the stale threshold");
        QVERIFY2(sawOldAndStale,
                 "no published snapshot exceeded the stale threshold within the drive window");

        // Other blocks keep succeeding: the snapshot that made the block under
        // test stale must still present the remaining blocks as fresh.
        if (c.blockIndex != 0)
            QVERIFY2(rig.snapshots.last().fast_quality != DataQuality::Stale,
                     "only the block without a successful transfer may go stale");
    }
}

// --- OB-7 ---------------------------------------------------------------------

void PlcM112D140SessionTest::stalledD140TriggersTheOfflineFreezePath()
{
    // OB-7: D140 change detection remains the liveness evidence; an unchanged
    // D140 must still trigger the offline/freeze path while every other block
    // keeps transferring healthily.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);
    advanceUntilOnline(*gw);
    QVERIFY2(gw->isOnline(), "precondition: the session must be online");

    // Precondition: healthy data keeps D140 changing.
    const quint16 healthyBefore = gw->model().readRegister(kD140);
    for (int i = 0; i < 6; ++i)
        gw->tick();
    QVERIFY2(gw->model().readRegister(kD140) != healthyBefore,
             "precondition: D140 must change while the session is healthy");
    QVERIFY2(gw->isOnline(), "precondition: a changing D140 keeps the session online");

    // Inject the heartbeat-freeze fault; the remaining machine data is healthy.
    gw->setHeartbeatFrozen(true);

    QVector<bool> connectionChanges;
    connect(gw, &SimulatedPlcGateway::connectionStateChanged, this,
            [&connectionChanges](quint64, bool online) {
                connectionChanges.append(online);
            });

    // The injected fault must actually hold D140 steady; otherwise the case
    // premise is invalid and must say so instead of passing vacuously.
    const quint16 frozenValue = gw->model().readRegister(kD140);
    for (int i = 0; i < 3; ++i) {
        gw->tick();
        QVERIFY2(gw->model().readRegister(kD140) == frozenValue,
                 "the injected heartbeat fault did not hold D140 steady");
    }

    // A D140 that stays unchanged past the liveness window must still drive the
    // session offline and must be observable to consumers.
    for (int i = 0; i < 20 && gw->isOnline(); ++i)
        gw->tick();
    QVERIFY2(!gw->isOnline(),
             "a stalled D140 must still trigger the offline/freeze path");
    QVERIFY2(!connectionChanges.isEmpty() && connectionChanges.last() == false,
             "the offline freeze must be observable as a connection state change");

    // Recovery: a resumed heartbeat restores the online session with valid data.
    gw->setHeartbeatFrozen(false);
    for (int i = 0; i < 10 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY2(gw->isOnline(),
             "a resumed D140 heartbeat must restore the online session");
    for (int i = 0; i < 5 && gw->lastSnapshot().overall_quality != DataQuality::Valid; ++i)
        gw->tick();
    QVERIFY2(gw->lastSnapshot().overall_quality == DataQuality::Valid,
             "a resumed heartbeat must republish valid data");

    app->shutdown();
}

// --- OB-8 ---------------------------------------------------------------------

void PlcM112D140SessionTest::failedTransferMarksOnlyItsBlockProtocolErrorUntilRefresh()
{
    // Contract quality rule: a failed transfer marks only its source block
    // ProtocolError and that block stays non-valid until a successful refresh.
    StaleRig rig(250, 250, 250, 250);
    QVERIFY2(rig.worker.isOnline(), "precondition: the poll worker must be online");

    // Drive a healthy window first so every block has a successful transfer:
    // only then is a ProtocolError on one block attributable to the injected
    // failure instead of a block that was never polled yet.
    for (int step = 0; step < 100; ++step) {
        rig.now += 50;
        rig.worker.onPollTick();
        while (rig.transport.sent.size() > rig.completed) {
            rig.transport.completeOk();
            ++rig.completed;
        }
        if (!rig.snapshots.isEmpty()
            && rig.snapshots.last().fast_quality == DataQuality::Valid
            && rig.snapshots.last().home_quality == DataQuality::Valid
            && rig.snapshots.last().command_quality == DataQuality::Valid
            && rig.snapshots.last().slow_quality == DataQuality::Valid)
            break;
    }
    QVERIFY2(!rig.snapshots.isEmpty(), "precondition: the gateway must publish snapshots");
    QVERIFY2(rig.snapshots.last().fast_quality == DataQuality::Valid
                 && rig.snapshots.last().home_quality == DataQuality::Valid
                 && rig.snapshots.last().command_quality == DataQuality::Valid
                 && rig.snapshots.last().slow_quality == DataQuality::Valid,
             "precondition: every block must be valid before one block is failed");

    // Inject failures for the home block only. Two failures also cover an
    // implementation-internal read retry; every other block keeps completing
    // successfully, so any ProtocolError published from here on is strictly the
    // home block's (the startup snapshot of a block that has not transferred
    // yet is not evidence and is excluded by this index).
    const int snapshotsBeforeInjection = rig.snapshots.size();
    const int homeFailuresToInject = 2;
    int injected = 0;
    for (int step = 0; step < 60 && injected < homeFailuresToInject; ++step) {
        rig.now += 50;
        rig.worker.onPollTick();
        while (rig.transport.sent.size() > rig.completed) {
            const ModbusRequest &request = rig.transport.sent[rig.completed];
            if (request.cls == RequestClass::HomePoll && injected < homeFailuresToInject) {
                ++injected;
                rig.transport.completeFail();
            } else {
                rig.transport.completeOk();
            }
            ++rig.completed;
        }
    }
    QVERIFY2(injected == homeFailuresToInject,
             "precondition: no home-block transfer was issued during the drive window");
    QVERIFY2(rig.worker.isOnline(),
             "injecting home-block transfer failures must not take the whole link offline");

    // Stop polling the home block: without a successful refresh the failed
    // block must stay non-valid while the other blocks keep succeeding.
    rig.worker.setPollIntervals(250, 1000000, 250, 250);
    for (int step = 0; step < 40; ++step) {
        rig.now += 50;
        rig.worker.onPollTick();
        while (rig.transport.sent.size() > rig.completed) {
            rig.transport.completeOk();
            ++rig.completed;
        }
    }

    int firstProtocolError = -1;
    for (int i = snapshotsBeforeInjection; i < rig.snapshots.size(); ++i) {
        if (rig.snapshots[i].home_quality == DataQuality::ProtocolError) {
            firstProtocolError = i;
            break;
        }
    }
    QVERIFY2(firstProtocolError >= 0,
             "a failed home transfer must mark the home block ProtocolError");
    for (int i = firstProtocolError; i < rig.snapshots.size(); ++i) {
        const DeviceSnapshot &s = rig.snapshots[i];
        QVERIFY2(s.home_quality != DataQuality::Valid,
                 "the failed block must stay non-valid until a successful refresh");
        QVERIFY2(s.fast_quality == DataQuality::Valid
                     && s.command_quality == DataQuality::Valid
                     && s.slow_quality == DataQuality::Valid,
                 qPrintable(QStringLiteral("a failed home transfer may mark only the home "
                                           "block (snapshot %1: fast=%2 home=%3 command=%4 "
                                           "slow=%5, ages %6/%7/%8/%9)")
                                .arg(i)
                                .arg(static_cast<int>(s.fast_quality))
                                .arg(static_cast<int>(s.home_quality))
                                .arg(static_cast<int>(s.command_quality))
                                .arg(static_cast<int>(s.slow_quality))
                                .arg(s.fast_age_ms)
                                .arg(s.home_age_ms)
                                .arg(s.command_age_ms)
                                .arg(s.slow_age_ms)));
    }

    // A later successful home transfer clears the ProtocolError.
    rig.worker.setPollIntervals(250, 250, 250, 250);
    for (int step = 0; step < 40; ++step) {
        rig.now += 50;
        rig.worker.onPollTick();
        while (rig.transport.sent.size() > rig.completed) {
            rig.transport.completeOk();
            ++rig.completed;
        }
        if (rig.snapshots.last().home_quality == DataQuality::Valid)
            break;
    }
    QVERIFY2(rig.snapshots.last().home_quality == DataQuality::Valid,
             "a later successful home transfer must clear the ProtocolError");
}

QTEST_MAIN(PlcM112D140SessionTest)
#include "plc_m112_d140_session_test.moc"
