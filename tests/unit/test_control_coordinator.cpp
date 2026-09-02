// Task 7 integration-style tests: ControlCoordinator (spec §10, §11.4, §13).
// Uses the deterministic SimulatedPlcGateway (Task 6): no serial, no sleeps.
//
// Coverage required by the task brief:
// - Permission matrix full combination (see test_permission_policy.cpp).
// - Each flow's preconditions, timeout and result convergence.
// - Logout clears M42/M106-M111 (not M100).
// - M100 is never auto-cleared.
// - No optimistic success: only snapshot-confirmed results are reported.

#include <QtTest>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "application/control_coordinator.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching AddressTable).
constexpr quint16 kM42 = 42;
constexpr quint16 kM100 = 100;
constexpr quint16 kM101 = 101;
constexpr quint16 kM102 = 102;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM105 = 105;
constexpr quint16 kM106 = 106;
constexpr quint16 kM107 = 107;
constexpr quint16 kM108 = 108;
constexpr quint16 kM109 = 109;
constexpr quint16 kM110 = 110;
constexpr quint16 kM111 = 111;
constexpr quint16 kD128 = 128;

} // namespace

class ControlCoordinatorTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // --- permission gating ---------------------------------------------------
    void anonymousCannotStartOrReset();
    void operatorCannotResetOrAdjust();
    void adminCanResetAndAdjust();

    // --- reset flow ----------------------------------------------------------
    void resetFromAutoModeWritesM104ThenPulsesM103();
    void resetWaitsForM61AndSucceeds();
    void resetTimeoutKeepsActualState();
    void resetRejectedWhenRunning();
    void resetDoesNotReportSuccessWithoutHomingStarted();

    // --- adjust width flow ---------------------------------------------------
    void adjustWidthWritesD128ThenPulsesM43();
    void adjustWidthSuccessConvergesOnM44();
    void adjustWidthFailureConvergesOnM45();
    void adjustWidthTargetEqualsCurrentSkipsM43();
    void adjustWidthRejectedWhenAdjusting();

    // --- start / stop --------------------------------------------------------
    void startWaitsForM3();
    void startRejectedWhenNotReady();
    void stopWaitsForM3Clear();
    void stopOfflineRejected();

    // --- estop ---------------------------------------------------------------
    void estopSetByAnyUser();
    void estopReleaseAdminOnly();
    void estopNotAutoClearedOnLogout();

    // --- manual / bypass -----------------------------------------------------
    void manualHoldWritesOnPressAndRelease();
    void manualLatchWrites();
    void bypassWrites();
    void manualHoldReleaseBypassesInterlocks();
    void estopSetSyncFailureDoesNotEmitSecondSuccess();

    // --- logout --------------------------------------------------------------
    void logoutClearsM42AndM106ToM111NotM100();
    void logoutClearDoesNotTouchM105();

    // --- timeout convergence -------------------------------------------------
    void adjustTimeoutConvergesToActualState();
    void startTimeoutConvergesToFailure();
    void stopTimeoutConvergesToFailure();
    void modeSwitchConvergesOnM1M2();
    void modeSwitchWriteFailureSurfaces();
    void modeSwitchTimeoutConverges();
    void estopReleaseConvergesViaM100ReadbackWhenM0Stuck();
    void manualAndBypassRejectUnsupportedAddress();
};

void ControlCoordinatorTest::init()
{
}

void ControlCoordinatorTest::cleanup()
{
}

// --- helpers ----------------------------------------------------------------

namespace {

// Drive a reset+home-return to a ready manual state via the raw gateway.
void homeReady(SimulatedPlcGateway &gw)
{
    gw.writeCoil(kM103, true);
    gw.writeCoil(kM103, false);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

// Forward declaration: see definition below (used by makeCoordinator).
ControlCoordinator *makeCoordinatorWithCoil(SimulatedPlcGateway &gw, qint64 &now,
                                            ControlCoordinator::PulseTransport t,
                                            ControlCoordinator::Config cfg);

// Build a coordinator wired to the simulated gateway. The pulse transport
// routes pulses straight into the gateway (the real worker thread would route
// them through the PulseStateMachine; the simulated gateway confirms writes by
// readback, so the pulse semantics are equivalent for these tests).
ControlCoordinator *makeCoordinator(SimulatedPlcGateway &gw, qint64 &now,
                                    ControlCoordinator::Config cfg = {})
{
    ControlCoordinator::PulseTransport t;
    t.startPulse = [&gw](quint16 a) {
        gw.writeCoil(a, true);
        gw.writeCoil(a, false);
        return true;
    };
    t.writeHold = [&gw](quint16 a, bool v) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) {
        gw.writeRegister(a, v);
        return true;
    };
    return makeCoordinatorWithCoil(gw, now, t, cfg);
}

// Like makeCoordinator but with a caller-supplied writeCoil transport (used to
// force write failures or no-op mode-select writes).
ControlCoordinator *makeCoordinatorWithCoil(
    SimulatedPlcGateway &gw, qint64 &now, ControlCoordinator::PulseTransport t,
    ControlCoordinator::Config cfg = {})
{
    auto *c = new ControlCoordinator(&gw, t, cfg, [&now]() { return now; });
    // Wire the gateway feed: snapshots, connection state and write results.
    QObject::connect(&gw, &SimulatedPlcGateway::snapshotReady, c,
                     [c](const DeviceSnapshot &s) { c->onSnapshot(s); });
    QObject::connect(&gw, &SimulatedPlcGateway::connectionStateChanged, c,
                     [c](bool online) { c->onConnectionChanged(online); });
    QObject::connect(&gw, &SimulatedPlcGateway::writeCompleted, c,
                     [c](quint16 a, bool ok, const QString &) { c->onWriteCompleted(a, ok); });
    // Feed the snapshot published before the coordinator existed.
    if (gw.hasSnapshot())
        c->onSnapshot(gw.lastSnapshot());
    return c;
}

// Like makeCoordinator but the startPulse transport is a no-op: the pulse is
// "sent" but the PLC never reacts (M3 never changes), for timeout tests.
ControlCoordinator *makeCoordinatorNoPulse(SimulatedPlcGateway &gw, qint64 &now,
                                           ControlCoordinator::Config cfg = {})
{
    ControlCoordinator::PulseTransport t;
    t.startPulse = [](quint16) { return true; }; // no-op: M3 stays put
    t.writeHold = [&gw](quint16 a, bool v) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) {
        gw.writeRegister(a, v);
        return true;
    };
    return makeCoordinatorWithCoil(gw, now, t, cfg);
}

} // namespace

// --- permission gating ------------------------------------------------------

void ControlCoordinatorTest::anonymousCannotStartOrReset()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));

    c->setRole(Role::Anonymous);
    QVERIFY(!c->start().accepted);
    QVERIFY(!c->reset().accepted);
    QVERIFY(!c->setMode(true).accepted);
    QVERIFY(!c->adjustWidth(300).accepted);
    QVERIFY(!c->estopRelease().accepted);
    QVERIFY(c->stop().accepted); // 未登录可停止
    QVERIFY(c->estopSet().accepted); // 未登录可置急停
}

void ControlCoordinatorTest::operatorCannotResetOrAdjust()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    homeReady(gw);
    gw.writeCoil(kM104, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());

    c->setRole(Role::Operator);
    QVERIFY(c->start().accepted);
    QVERIFY(!c->reset().accepted);
    QVERIFY(!c->adjustWidth(300).accepted);
    QVERIFY(!c->setMode(true).accepted);
    QVERIFY(!c->estopRelease().accepted);
    QVERIFY(!c->manualHold(kM106, true).accepted);
    QVERIFY(!c->bypass(kM110, true).accepted);
}

void ControlCoordinatorTest::adminCanResetAndAdjust()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    homeReady(gw);

    c->setRole(Role::Admin);
    QVERIFY(c->reset().accepted);
    QVERIFY(c->setMode(true).accepted);
    QVERIFY(c->manualHold(kM106, true).accepted);
    QVERIFY(c->bypass(kM110, true).accepted);
}

// --- reset flow -------------------------------------------------------------

void ControlCoordinatorTest::resetFromAutoModeWritesM104ThenPulsesM103()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // Put the machine into auto mode first.
    gw.writeCoil(kM104, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());

    QVERIFY(c->reset().accepted);
    // M104=0 written (auto -> manual), then M103 pulse.
    QVERIFY(!gw.model().readCoil(kM104));
    gw.tick();
    QVERIFY(gw.lastSnapshot().m1()); // manual mode
    gw.tick(); // the M103 pulse fired during the previous snapshot's processing
    QVERIFY(gw.lastSnapshot().m50()); // homing
    QVERIFY(c->resetInProgress());
}

void ControlCoordinatorTest::resetWaitsForM61AndSucceeds()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    bool result = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Reset) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->reset().accepted);
    gw.tick(); // homing in progress
    QVERIFY(c->resetInProgress());
    QVERIFY(!result); // no optimistic success

    gw.tick(); // home return completes: M61=1, M50=0
    QVERIFY(gw.lastSnapshot().m9()); // M61 via M9
    QVERIFY(!gw.lastSnapshot().m50());
    QVERIFY(result);
    QVERIFY(!c->resetInProgress());
}

void ControlCoordinatorTest::resetTimeoutKeepsActualState()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::Config cfg;
    cfg.resetTimeoutSec = 30;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now, cfg));
    c->setRole(Role::Admin);

    bool result = true; // must flip to false
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Reset) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->reset().accepted);
    gw.tick(); // homing in progress

    // Advance the injected clock past the timeout without the PLC homing.
    now += 31'000;
    gw.tick();
    QVERIFY(!result); // timeout: not optimistic success
    QVERIFY(!c->resetInProgress());
    QVERIFY(detail.contains(QStringLiteral("超时")));
}

void ControlCoordinatorTest::resetRejectedWhenRunning()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // Run the machine.
    homeReady(gw);
    gw.writeCoil(kM104, true);
    gw.tick();
    gw.writeCoil(kM101, true);
    gw.writeCoil(kM101, false);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3());

    QVERIFY(!c->reset().accepted); // M3=1: 禁止复位
}

void ControlCoordinatorTest::resetDoesNotReportSuccessWithoutHomingStarted()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::Config cfg;
    cfg.resetTimeoutSec = 30;
    // No-op startPulse: the M103 pulse is "sent" but never delivered to the
    // PLC, so M50 never rises (lost-pulse scenario).
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorNoPulse(gw, now, cfg));
    c->setRole(Role::Admin);

    // Machine already homed (M61=1) with a latched fault (M14=1).
    homeReady(gw);
    gw.writeCoil(kM100, true); // estop latches M14=1, D110=1
    gw.tick();
    QVERIFY(gw.lastSnapshot().m9()); // M61 via M9
    QVERIFY(gw.lastSnapshot().m14());

    bool result = true; // must not stay true
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Reset) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->reset().accepted);
    gw.tick(); // M103 lost: M50 never rises, M61 stays 1, M14 stays 1

    // The flow must NOT report success: the reset never executed and the
    // fault was never cleared. It converges to failure (fault check) rather
    // than an optimistic "回原点完成".
    QVERIFY(!result);
    QVERIFY(!c->resetInProgress());
    QVERIFY(detail.contains(QStringLiteral("故障")));
    QVERIFY(gw.lastSnapshot().m14()); // fault not reported cleared
}

// --- adjust width flow ------------------------------------------------------

void ControlCoordinatorTest::adjustWidthWritesD128ThenPulsesM43()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVERIFY(c->adjustWidth(300).accepted);
    QCOMPARE(gw.model().readRegister(kD128), quint16(300)); // D128 written
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34()); // adjusting
    QVERIFY(c->adjustInProgress());
    QCOMPARE(c->adjustTarget().value_or(0), quint16(300));
    QCOMPARE(c->adjustStartWidth().value_or(0), quint16(200));
    QCOMPARE(c->adjustSpeed().value_or(0), quint16(15));
}

void ControlCoordinatorTest::adjustWidthSuccessConvergesOnM44()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    bool result = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::AdjustWidth) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick(); // M34=1
    QVERIFY(!result); // no optimistic success

    // ceil(100/15) = 7 s to complete.
    for (int i = 0; i < 7; ++i)
        gw.tick();
    QVERIFY(gw.lastSnapshot().m44());
    QVERIFY(!gw.lastSnapshot().m45());
    QCOMPARE(gw.lastSnapshot().currentWidth(), quint16(300));
    QVERIFY(result);
    QVERIFY(!c->adjustInProgress());
}

void ControlCoordinatorTest::adjustWidthFailureConvergesOnM45()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    bool result = true;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::AdjustWidth) {
                    result = ok;
                    detail = d;
                }
            });

    // Stall the motor: positioning never completes -> M45 + fault 10.
    gw.model().setPositioningStall(true);
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(c->adjustInProgress());

    // Timeout = ceil(100/15) + 5 = 12 s.
    for (int i = 0; i < 12; ++i)
        gw.tick();
    QVERIFY(gw.lastSnapshot().m45());
    QVERIFY(!gw.lastSnapshot().m44());
    QVERIFY(!result); // failure reported, not optimistic
    QVERIFY(!c->adjustInProgress());
}

void ControlCoordinatorTest::adjustWidthTargetEqualsCurrentSkipsM43()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    bool result = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::AdjustWidth) {
                    result = ok;
                    detail = d;
                }
            });

    // Target == current (200): no M43 pulse, immediate "已是目标宽度".
    QVERIFY(c->adjustWidth(200).accepted);
    QVERIFY(!gw.model().readCoil(43)); // no M43 write
    QVERIFY(result);
    QVERIFY(detail.contains(QStringLiteral("已是目标宽度")));
    QVERIFY(!c->adjustInProgress());
}

void ControlCoordinatorTest::adjustWidthRejectedWhenAdjusting()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34());

    // M34=1: 禁止再次写 D128 或发送 M43 (spec §10.3 step 6).
    QVERIFY(!c->adjustWidth(350).accepted);
}

// --- start / stop -----------------------------------------------------------

void ControlCoordinatorTest::startWaitsForM3()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Operator);
    homeReady(gw);
    gw.writeCoil(kM104, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());

    bool result = false;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Start)
                    result = ok;
            });

    QVERIFY(c->start().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3()); // running
    QVERIFY(result); // only after M3=1 observed
    QVERIFY(!c->startInProgress());
}

void ControlCoordinatorTest::startRejectedWhenNotReady()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Operator);

    // Not homed, not auto: start rejected with interlock reasons.
    const ControlCoordinator::CommandResult r = c->start();
    QVERIFY(!r.accepted);
    QVERIFY(!r.reason.isEmpty());
}

void ControlCoordinatorTest::stopWaitsForM3Clear()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Anonymous);
    homeReady(gw);
    gw.writeCoil(kM104, true);
    gw.tick();
    gw.writeCoil(kM101, true);
    gw.writeCoil(kM101, false);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3());

    bool result = false;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Stop)
                    result = ok;
            });

    QVERIFY(c->stop().accepted);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m3());
    QVERIFY(result); // only after M3=0 observed
    QVERIFY(!c->stopInProgress());
}

void ControlCoordinatorTest::stopOfflineRejected()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Anonymous);

    gw.setLinkDown(true);
    QVERIFY(!gw.isOnline());
    const ControlCoordinator::CommandResult r = c->stop();
    QVERIFY(!r.accepted);
    QVERIFY(r.reason.contains(QStringLiteral("通讯中断")));
}

// --- estop ------------------------------------------------------------------

void ControlCoordinatorTest::estopSetByAnyUser()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Anonymous);

    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m100());
}

void ControlCoordinatorTest::estopReleaseAdminOnly()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Operator);

    gw.writeCoil(kM100, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());

    // Operator cannot release.
    QVERIFY(!c->estopRelease().accepted);

    // Admin can.
    c->setRole(Role::Admin);
    QVERIFY(c->estopRelease().accepted);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m0());
    // 解除成功≠设备可运行: latched fault stays until reset.
    QVERIFY(gw.lastSnapshot().m14());
}

void ControlCoordinatorTest::estopNotAutoClearedOnLogout()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m100());

    c->logoutClear();
    gw.tick();
    QVERIFY(gw.lastSnapshot().m100()); // M100 never auto-cleared
    QVERIFY(gw.lastSnapshot().m0());
}

// --- manual / bypass --------------------------------------------------------

void ControlCoordinatorTest::manualHoldWritesOnPressAndRelease()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVERIFY(c->manualHold(kM106, true).accepted);
    QVERIFY(gw.model().readCoil(kM106));
    QVERIFY(c->manualHold(kM106, false).accepted);
    QVERIFY(!gw.model().readCoil(kM106));
}

void ControlCoordinatorTest::manualLatchWrites()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVERIFY(c->manualLatch(kM109, true).accepted);
    QVERIFY(gw.model().readCoil(kM109));
    QVERIFY(c->manualLatch(kM109, false).accepted);
    QVERIFY(!gw.model().readCoil(kM109));
}

void ControlCoordinatorTest::bypassWrites()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    QVERIFY(c->bypass(kM110, true).accepted);
    QVERIFY(gw.model().readCoil(kM110));
    QVERIFY(c->bypass(kM110, false).accepted);
    QVERIFY(!gw.model().readCoil(kM110));
}

// Release (write 0) must bypass machine-state interlocks: if a fault/estop
// latches while the button is held, the release is still sent so the
// continuous command clears immediately (spec §10.7 松开写 0, §13).
void ControlCoordinatorTest::manualHoldReleaseBypassesInterlocks()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    // Press M106 (gated, accepted).
    QVERIFY(c->manualHold(kM106, true).accepted);
    QVERIFY(gw.model().readCoil(kM106));

    // Latch an estop: M0=1, M14=1 -> the manual interlock now rejects a press.
    gw.writeCoil(kM100, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m14());
    QVERIFY(!c->manualHold(kM106, true).accepted); // press now rejected

    // Release must still be sent (write 0), not rejected by the interlocks.
    QVERIFY(c->manualHold(kM106, false).accepted);
    QVERIFY(!gw.model().readCoil(kM106));
}

// A synchronous estop-set write failure must clear the pending flag so a later
// snapshot (M0=1) cannot emit a second, contradictory success (spec §13).
void ControlCoordinatorTest::estopSetSyncFailureDoesNotEmitSecondSuccess()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t;
    t.writeCoil = [](quint16, bool, CommandPriority) { return false; };
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorWithCoil(gw, now, t));
    c->setRole(Role::Admin);

    int successCount = 0;
    int failureCount = 0;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::EstopSet) {
                    if (ok)
                        ++successCount;
                    else
                        ++failureCount;
                }
            });

    // The M100 write fails synchronously -> immediate failure, no pending.
    QVERIFY(c->estopSet().accepted);
    QCOMPARE(failureCount, 1);
    QCOMPARE(successCount, 0);

    // A later snapshot with M0=1 (physical estop) must NOT emit a second success.
    gw.writeCoil(kM100, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QCOMPARE(successCount, 0);
    QCOMPARE(failureCount, 1);
}

// --- logout --------------------------------------------------------------

void ControlCoordinatorTest::logoutClearsM42AndM106ToM111NotM100()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // Set all the continuous/bypass bits.
    gw.writeCoil(kM42, true);
    for (quint16 a = kM106; a <= kM111; ++a)
        gw.writeCoil(a, true);
    gw.writeCoil(kM100, true); // estop: must survive logout
    gw.tick();
    QVERIFY(gw.lastSnapshot().m42());
    QVERIFY(gw.lastSnapshot().m106());
    QVERIFY(gw.lastSnapshot().m111());
    QVERIFY(gw.lastSnapshot().m100());

    c->logoutClear();
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m42());
    QVERIFY(!gw.lastSnapshot().m106());
    QVERIFY(!gw.lastSnapshot().m107());
    QVERIFY(!gw.lastSnapshot().m108());
    QVERIFY(!gw.lastSnapshot().m109());
    QVERIFY(!gw.lastSnapshot().m110());
    QVERIFY(!gw.lastSnapshot().m111());
    QVERIFY(gw.lastSnapshot().m100()); // M100 not touched
}

void ControlCoordinatorTest::logoutClearDoesNotTouchM105()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    gw.writeCoil(kM105, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m105());

    c->logoutClear();
    gw.tick();
    QVERIFY(gw.lastSnapshot().m105()); // M105 模式选择保持不变
}

// --- timeout convergence -----------------------------------------------------

void ControlCoordinatorTest::adjustTimeoutConvergesToActualState()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    bool result = true;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::AdjustWidth) {
                    result = ok;
                    detail = d;
                }
            });

    // Stall: the PLC will time out on its own (12 s). The HMI defensive
    // timeout (plc_timeout + 3 = 15 s) must not fire first, and the result
    // must converge to the actual M45 state.
    gw.model().setPositioningStall(true);
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(c->adjustInProgress());

    for (int i = 0; i < 12; ++i)
        gw.tick();
    QVERIFY(gw.lastSnapshot().m45());
    QVERIFY(!result); // converged to failure
    QVERIFY(!c->adjustInProgress());
}

// Start: M3 never becomes 1 -> HMI defensive timeout converges to failure,
// never stuck in "pending".
void ControlCoordinatorTest::startTimeoutConvergesToFailure()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorNoPulse(gw, now));
    c->setRole(Role::Operator);
    homeReady(gw);
    gw.writeCoil(kM104, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());

    bool result = true;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Start) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->start().accepted);
    gw.tick(); // M101 "sent" (no-op); the PLC never sets M3=1
    QVERIFY(c->startInProgress());

    // Advance the injected clock past kStartStopTimeoutMs (10 s).
    now += 10'001;
    gw.tick();
    QVERIFY(!result); // timeout failure, not optimistic
    QVERIFY(!c->startInProgress());
    QVERIFY(detail.contains(QStringLiteral("超时")));
}

// Stop: M3 never becomes 0 -> HMI defensive timeout converges to failure.
void ControlCoordinatorTest::stopTimeoutConvergesToFailure()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorNoPulse(gw, now));
    c->setRole(Role::Anonymous);
    homeReady(gw);
    gw.writeCoil(kM104, true);
    gw.tick();
    gw.writeCoil(kM101, true);
    gw.writeCoil(kM101, false);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3());

    bool result = true;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Stop) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->stop().accepted);
    gw.tick(); // M102 "sent" (no-op); the PLC never clears M3
    QVERIFY(c->stopInProgress());

    now += 10'001;
    gw.tick();
    QVERIFY(!result); // timeout failure, not optimistic
    QVERIFY(!c->stopInProgress());
    QVERIFY(detail.contains(QStringLiteral("超时")));
}

// Mode switch: M104 write succeeds, M1/M2 reflects -> converges to success.
void ControlCoordinatorTest::modeSwitchConvergesOnM1M2()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    bool result = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::ModeSwitch) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->setMode(true).accepted); // -> auto (M2)
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());
    QVERIFY(result);
    QVERIFY(detail.contains(QStringLiteral("自动")));

    result = false;
    QVERIFY(c->setMode(false).accepted); // -> manual (M1)
    gw.tick();
    QVERIFY(gw.lastSnapshot().m1());
    QVERIFY(result);
    QVERIFY(detail.contains(QStringLiteral("手动")));
}

// Mode switch: a failed M104 write surfaces as a write failure (not timeout).
void ControlCoordinatorTest::modeSwitchWriteFailureSurfaces()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t;
    t.writeCoil = [](quint16, bool, CommandPriority) { return false; };
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorWithCoil(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    bool result = true;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::ModeSwitch) {
                    result = ok;
                    detail = d;
                }
            });

    // The M104 write fails (writeCoil returns false).
    QVERIFY(c->setMode(true).accepted);
    QVERIFY(!result); // immediate write failure reported
    QVERIFY(detail.contains(QStringLiteral("失败")));
}

// Mode switch: M104 write succeeds but M1/M2 never reflects -> timeout.
void ControlCoordinatorTest::modeSwitchTimeoutConverges()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t;
    t.startPulse = [&gw](quint16 a) {
        gw.writeCoil(a, true);
        gw.writeCoil(a, false);
        return true;
    };
    t.writeHold = [&gw](quint16 a, bool v) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) {
        gw.writeRegister(a, v);
        return true;
    };
    t.writeCoil = [](quint16, bool, CommandPriority) { return true; }; // no-op select
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorWithCoil(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    bool result = true;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::ModeSwitch) {
                    result = ok;
                    detail = d;
                }
            });

    QVERIFY(c->setMode(true).accepted); // write "succeeds" but M1/M2 never reflects
    QVERIFY(!gw.lastSnapshot().m2());

    now += 5'001; // past kModeTimeoutMs
    gw.tick();
    QVERIFY(!result); // timeout failure, not stuck
    QVERIFY(detail.contains(QStringLiteral("超时")));
}

// Estop release with a physical estop stuck (M0=1): the M100 readback confirms
// the release write took effect -> converges, does not hang on 待确认.
void ControlCoordinatorTest::estopReleaseConvergesViaM100ReadbackWhenM0Stuck()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // Set the estop and inject a stuck physical estop that keeps M0=1 even
    // after the HMI clears M100 (the M100 readback still confirms the write).
    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    gw.model().setEstopReleaseStuck(true);

    bool result = false;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::EstopRelease)
                    result = ok;
            });

    QVERIFY(c->estopRelease().accepted);
    gw.tick();
    // M0 still 1 (physical estop), but M100 readback shows the release write.
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(!gw.lastSnapshot().m100());
    QVERIFY(result); // converged via M100 readback, not hung
}

// manualHold / manualLatch / bypass reject addresses outside the spec whitelist.
void ControlCoordinatorTest::manualAndBypassRejectUnsupportedAddress()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    // manualHold: only M106/M107/M108.
    QVERIFY(!c->manualHold(kM109, true).accepted);
    QVERIFY(!c->manualHold(0, true).accepted);
    // manualLatch: only M109.
    QVERIFY(!c->manualLatch(kM106, true).accepted);
    // bypass: only M42/M105/M110/M111.
    QVERIFY(!c->bypass(kM106, true).accepted);
    QVERIFY(!c->bypass(kM100, true).accepted);
    QVERIFY(!gw.model().readCoil(kM109)); // nothing was written
    QVERIFY(!gw.model().readCoil(kM106));
    QVERIFY(!gw.model().readCoil(kM100));
}

QTEST_GUILESS_MAIN(ControlCoordinatorTest)
#include "test_control_coordinator.moc"
