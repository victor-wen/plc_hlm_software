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
constexpr quint16 kM50 = 50;
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
constexpr quint16 kD204 = 204;

quint64 nextRequestId()
{
    static quint64 next = 1;
    return next++;
}

SubmissionResult acceptedResult()
{
    SubmissionResult r;
    r.accepted = true;
    r.request_id = nextRequestId();
    r.gateway_generation = 1;
    return r;
}

SubmissionResult rejectedResult(const QString &reason)
{
    SubmissionResult r;
    r.accepted = false;
    r.request_id = 0;
    r.gateway_generation = 1;
    r.immediate_rejection_reason = reason;
    return r;
}

// PLC-HMI-010/011: records reset submissions and answers with correlated
// accepted results without mutating any PLC state, so the reset result can be
// observed against independently injected synthetic snapshots. The test
// delivers the recorded completion explicitly (the reset needs the M103 pulse's
// correlated outcome; 回原点 is a separate command since 2026-09-21).
struct ResetRecorder
{
    struct Recorded
    {
        PlcOperation operation = PlcOperation::Pulse;
        quint16 address = 0;
        bool value = false;
        quint64 request_id = 0;
        quint64 gateway_generation = 0;
    };

    QVector<quint16> pulses;
    QVector<Recorded> submissions;

    ControlCoordinator::PulseTransport make()
    {
        ControlCoordinator::PulseTransport t;
        t.startPulse = [this](quint16 address) -> SubmissionResult {
            pulses.append(address);
            return record(PlcOperation::Pulse, address, false);
        };
        t.writeHold = [](quint16, bool) { return acceptedResult(); };
        t.writeCoil = [this](quint16 address, bool value, CommandPriority) {
            return record(PlcOperation::WriteCoil, address, value);
        };
        t.writeRegister = [](quint16, quint16, CommandPriority) {
            return acceptedResult();
        };
        return t;
    }

    SubmissionCompletion completionFor(const Recorded &r, bool ok) const
    {
        SubmissionCompletion c;
        c.request_id = r.request_id;
        c.gateway_generation = r.gateway_generation;
        c.operation = r.operation;
        c.address = r.address;
        c.result = ok;
        if (!ok)
            c.error = QStringLiteral("submission failed");
        return c;
    }

private:
    SubmissionResult record(PlcOperation operation, quint16 address, bool value)
    {
        const SubmissionResult r = acceptedResult();
        submissions.append({operation, address, value, r.request_id,
                            r.gateway_generation});
        return r;
    }
};

// Synthetic not-running-machine snapshot (PLC-HMI-010 D2). homeBits carries M50
// (bit 0); statusWord1 carries M1 (manual), M9 (home complete) and M14
// (latched fault); faultCode is D110. Per-block quality is Valid so every field
// is usable evidence.
DeviceSnapshot syntheticResetSnapshot(bool m50, bool m9, bool m14,
                                      quint16 faultCode)
{
    DeviceSnapshotData d;
    d.connected = true;
    quint16 sw1 = quint16(1) << 1; // M1 manual
    if (m9)
        sw1 |= quint16(1) << 9; // M9 home complete
    if (m14)
        sw1 |= quint16(1) << 14; // M14 latched fault
    d.statusWord1 = sw1;
    d.homeBits = m50 ? quint16(1) : quint16(0);
    d.faultCode = faultCode;
    d.targetWidth = 200;
    d.currentWidth = 200;
    d.widthDelta = 0;
    d.pulsePerMm = 128;
    d.widthSpeed = 15;
    d.beltSpeed = 5000;
    d.heartbeat = 1;
    d.fast_quality = DataQuality::Valid;
    d.fast_age_ms = 0;
    d.home_quality = DataQuality::Valid;
    d.home_age_ms = 0;
    d.command_quality = DataQuality::Valid;
    d.command_age_ms = 0;
    d.slow_quality = DataQuality::Valid;
    d.slow_age_ms = 0;
    d.overall_quality = aggregateQuality(d);
    return DeviceSnapshot(d);
}

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

    // --- reset flow (PLC-HMI-010 fire-and-confirm-by-fixed-delay) -----------
    void resetFromAutoModePulsesM103WithoutWritingM104();
    void resetConvergesAfterFixedDelayNotBefore();
    void resetConvergesWhenPulseAcceptedButHomeNeverStarts();
    void resetResultIgnoresHomeBitsAndFaultCode();
    void resetRejectedWhenRunning();

    // --- adjust width flow ---------------------------------------------------
    void adjustWidthWritesD128ThenPulsesM43();
    void adjustWidthSuccessConvergesOnM44();
    void adjustWidthFailureConvergesOnM45();
    void adjustWidthTargetEqualsCurrentSkipsM43();
    void adjustWidthRejectedWhenAdjusting();
    void adjustWidthConcurrentEstopNeverHangs();

    // --- start / stop --------------------------------------------------------
    void startWaitsForM3();
    void startRejectedWhenNotReady();
    void stopWaitsForM3Clear();
    void stopOfflineRejected();

    // --- estop ---------------------------------------------------------------
    void estopSetByAnyUser();
    void estopReleaseAdminOnly();
    void estopNotAutoClearedOnLogout();
    void estopSetDuringReleaseConverges();
    void estopSetTimeoutConverges();
    void estopReleaseTimeoutConverges();

    // --- manual / bypass -----------------------------------------------------
    void manualHoldWritesOnPressAndRelease();
    void manualLatchWrites();
    void bypassWrites();
    void manualHoldReleaseBypassesInterlocks();
    void manualHoldReleaseRequiresPermission();
    void estopSetSyncFailureDoesNotEmitSecondSuccess();

    // --- 测试信号 M114-M117 (user decision 2026-09-22) -----------------------
    void simStationPulseIsAdminOnlyAndConvergesVisibly();

    // --- logout --------------------------------------------------------------
    void logoutClearsM42AndM106ToM111NotM100();
    void logoutClearDoesNotTouchM105();

    // --- timeout convergence -------------------------------------------------
    void adjustTimeoutConvergesToActualState();
    void adjustDefensiveDeadlineIsPlcTimeoutPlusThreeSeconds();
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
// PLC-HMI-011 D6: the M103 pulse no longer starts homing; the HMI's single
// sustained M50=1 home-start write does.
//
// The decoded SBR_HOME completion zeroes the current width (`DMOV K0 D130`),
// and the decoded M60 rung is M61 ∧ D128==D130 ∧ ¬M0 ∧ ¬M14 ∧ ¬T6 — so a homed
// machine is not 自动准备完成 until one width adjust actually reaches the
// target (user decision 2026-09-22). D128 defaults to 200, D204 to 128 and D220
// to the clamped 15, so the run takes ceil(200 * 128 / (15 * 1280)) = 2 s and
// leaves D130 at 200, the state these tests were written against.
void homeReady(SimulatedPlcGateway &gw)
{
    // The M43 preconditions include manual mode M1, so a test that switched to
    // auto mode before calling this must be put back first.
    gw.model().writeCoil(kM104, false);
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.model().writeCoil(kM50, true);
    gw.tick();
    gw.tick(); // home return takes 2 s
    gw.model().writeCoil(43, true);
    gw.model().writeCoil(43, false);
    gw.tick();
    gw.tick(); // width adjust takes 2 s
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
    // Route every submission through the gateway's submit API so its
    // submission bookkeeping runs and the correlated submissionCompleted is
    // emitted on tick() (PLC-HMI-011: the reset only converges when both the
    // M103 pulse and the M50 home-start write have correlated completions).
    t.startPulse = [&gw](quint16 a) -> SubmissionResult { return gw.submitPulse(a); };
    t.writeHold = [&gw](quint16 a, bool v) -> SubmissionResult {
        return gw.submitWriteCoil(a, v);
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority p) -> SubmissionResult {
        return gw.submitWriteCoil(a, v, p);
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority p) -> SubmissionResult {
        return gw.submitWriteRegister(a, v, p);
    };
    return makeCoordinatorWithCoil(gw, now, t, cfg);
}

// Like makeCoordinator but with a caller-supplied writeCoil transport (used to
// force write failures or no-op mode-select writes).
ControlCoordinator *makeCoordinatorWithCoil(
    SimulatedPlcGateway &gw, qint64 &now, ControlCoordinator::PulseTransport t,
    ControlCoordinator::Config cfg = {})
{
    auto *c = new ControlCoordinator(t, cfg, [&now]() { return now; });
    // Wire the gateway feed: snapshots, connection state and write results.
    QObject::connect(&gw, &SimulatedPlcGateway::snapshotReady, c,
                     [c](quint64, const DeviceSnapshot &s) { c->onSnapshot(s); });
    QObject::connect(&gw, &SimulatedPlcGateway::connectionStateChanged, c,
                     [c](quint64, bool online) { c->onConnectionChanged(online); });
    QObject::connect(&gw, &SimulatedPlcGateway::submissionCompleted, c,
                     [c](const SubmissionCompletion &completion) {
                         c->onSubmissionCompleted(completion);
                     });
    // Feed the snapshot published before the coordinator existed.
    if (gw.hasSnapshot())
        c->onSnapshot(gw.lastSnapshot());
    return c;
}

// Like makeCoordinator but the startPulse transport is a no-op: the pulse is
// "sent" but the PLC never reacts (M3 never changes), for timeout tests.
// The coil/register writes still route through the gateway's submit API so
// their correlated completions arrive.
ControlCoordinator *makeCoordinatorNoPulse(SimulatedPlcGateway &gw, qint64 &now,
                                           ControlCoordinator::Config cfg = {})
{
    ControlCoordinator::PulseTransport t;
    t.startPulse = [](quint16) -> SubmissionResult {
        return acceptedResult(); // no-op: M3 stays put
    };
    t.writeHold = [&gw](quint16 a, bool v) -> SubmissionResult {
        return gw.submitWriteCoil(a, v);
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority p) -> SubmissionResult {
        return gw.submitWriteCoil(a, v, p);
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority p) -> SubmissionResult {
        return gw.submitWriteRegister(a, v, p);
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
    gw.model().writeCoil(kM104, true);
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
    QVERIFY(c->resetInProgress());

    // PLC-HMI-010 D1 supersedes the PLC-HMI-001 M104-sharing conflict: the
    // reset no longer writes M104, so a mode switch and a reset do not share a
    // coil and may overlap. The overlap still produces a visible signal (an
    // accepted mode switch), never a silent return.
    QSignalSpy accepted(c.get(), &ControlCoordinator::commandAccepted);
    QVERIFY(c->setMode(true).accepted);
    QCOMPARE(accepted.count(), 1);
    QCOMPARE(accepted[0][0].value<Command>(), Command::ModeSwitch);

    // User decision 2026-09-21: the reset no longer homes the machine. The M103
    // pulse completion plus the fixed 200 ms boundary converge it, and the
    // machine is deliberately left un-homed.
    gw.tick();
    now += ControlCoordinator::kResetCompletionDelayMs;
    gw.tick();
    QVERIFY(!c->resetInProgress());
    QVERIFY2(!gw.lastSnapshot().m9(),
             "the reset started homing although 回原点 is a separate command");

    // Return to manual mode, then home the machine with the dedicated 回原点
    // command: exactly one sustained M50=1 write starts the PLC home return, and
    // the PLC clears M50 itself and sets M61/M9 when it completes.
    QVERIFY(c->setMode(false).accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m1()); // manual mode

    QVERIFY2(c->homeStart().accepted, "the 回原点 command was not accepted");
    gw.tick();
    QVERIFY2(gw.model().readCoil(kM50), "the 回原点 command did not write the home-start coil");
    gw.tick();
    gw.tick(); // home return takes 2 s
    QVERIFY(gw.lastSnapshot().m9()); // M61 via M9: homed

    QVERIFY(c->manualHold(kM106, true).accepted);
    QVERIFY(c->bypass(kM110, true).accepted);
}

// --- reset flow (PLC-HMI-010 fire-and-confirm-by-fixed-delay) ---------------

void ControlCoordinatorTest::resetFromAutoModePulsesM103WithoutWritingM104()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // Put the machine into auto mode first.
    gw.model().writeCoil(kM104, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());
    QVERIFY(gw.model().readCoil(kM104));

    QVERIFY(c->reset().accepted);
    // PLC-HMI-010 D1: the reset sends the M103 pulse only. It never writes M104
    // (no auto -> manual switch) and never waits for M1=1, so the mode coil is
    // left exactly where it was.
    QVERIFY2(gw.model().readCoil(kM104), "the reset wrote M104");
    QVERIFY(c->resetInProgress());

    // User decision 2026-09-21: the reset also never writes the home-start coil
    // (M50). Homing is started by the separate 回原点 command, so the reset
    // leaves the machine un-homed and the coil untouched.
    gw.tick();
    QVERIFY2(!gw.model().readCoil(kM50), "the reset wrote the home-start coil M50");
    QVERIFY(c->resetInProgress());

    now += ControlCoordinator::kResetCompletionDelayMs;
    gw.tick();
    QVERIFY(!c->resetInProgress());
    QVERIFY2(!gw.lastSnapshot().m50(), "the reset started homing");
}

void ControlCoordinatorTest::resetConvergesAfterFixedDelayNotBefore()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    int terminals = 0;
    bool lastOk = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Reset) {
                    ++terminals;
                    lastOk = ok;
                    detail = d;
                }
            });

    QVERIFY(c->reset().accepted);
    QVERIFY(c->resetInProgress());

    // PLC-HMI-010 D3: the fixed completion delay is 200 ms from the M103 pulse
    // submission; the result may not arrive before that boundary.
    gw.tick();
    QCOMPARE(terminals, 0); // no optimistic success

    now += ControlCoordinator::kResetCompletionDelayMs - 1;
    gw.tick();
    QCOMPARE(terminals, 0);
    QVERIFY(c->resetInProgress());

    now += 1;
    gw.tick();
    QCOMPARE(terminals, 1);
    QVERIFY(lastOk);
    QCOMPARE(detail, QStringLiteral("复位完成"));
    QVERIFY(!c->resetInProgress());

    // Exactly one terminal: a later snapshot must not emit a second result.
    gw.tick();
    now += 5'000;
    gw.tick();
    QCOMPARE(terminals, 1);
}

void ControlCoordinatorTest::resetConvergesWhenPulseAcceptedButHomeNeverStarts()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    // No-op startPulse: the M103 pulse is accepted but never delivered to the
    // PLC, so its correlated completion never arrives and the M50 home-start
    // write is never issued (lost-pulse scenario).
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorNoPulse(gw, now));
    c->setRole(Role::Admin);

    // Machine already homed (M61=1 via M9) with a latched fault (M14=1).
    homeReady(gw);
    gw.model().writeCoil(kM100, true); // estop latches M14=1, D110=1
    gw.tick();
    QVERIFY(gw.lastSnapshot().m9()); // M61 via M9
    QVERIFY(gw.lastSnapshot().m14());

    int terminals = 0;
    bool lastOk = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Reset) {
                    ++terminals;
                    lastOk = ok;
                    detail = d;
                }
            });

    QVERIFY(c->reset().accepted);
    QVERIFY(c->resetInProgress());
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m50()); // the lost pulse never started homing
    QCOMPARE(terminals, 0);

    // PLC-HMI-011 D2/D3: a lost pulse means the reset can never converge to
    // success. It converges to exactly one visible non-success inside the
    // home-start confirmation deadline, and never reports 复位完成.
    now += ControlCoordinator::kHomeStartConfirmTimeoutMs;
    gw.tick();
    QCOMPARE(terminals, 1);
    QVERIFY(!lastOk);
    QVERIFY(!detail.isEmpty());
    QVERIFY(detail != QStringLiteral("复位完成"));
    QVERIFY(!c->resetInProgress());
    QVERIFY(gw.lastSnapshot().m14()); // the HMI result never pretends a fault cleared

    gw.tick();
    QCOMPARE(terminals, 1);
}

void ControlCoordinatorTest::resetResultIgnoresHomeBitsAndFaultCode()
{
    // PLC-HMI-010 D2: the reset result must be identical whether the snapshot
    // shows M50 low/high, M61 (M9) clear/set, M14 set/clear, or D110 zero or
    // non-zero. The machine state is injected as synthetic snapshots so each
    // bit is varied independently of the simulator's own flag handling.
    struct Case
    {
        QString name;
        bool m50;
        bool m9;
        bool m14;
        quint16 faultCode;
    };
    const QVector<Case> cases{
        {QStringLiteral("M50 low/M9 clear/no fault"), false, false, false, 0},
        {QStringLiteral("M50 high"), true, true, false, 0},
        {QStringLiteral("M9 set with M14 latched"), false, true, true, 1},
        {QStringLiteral("D110 non-zero"), false, true, false, 9},
        {QStringLiteral("M50 high/M9 clear/M14 latched/D110=7"), true, false, true, 7},
    };

    QVector<QPair<bool, QString>> results;
    for (const Case &k : cases) {
        ResetRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(
            new ControlCoordinator(rec.make(), ControlCoordinator::Config(),
                                   [&now]() { return now; }));
        c->setRole(Role::Admin);
        c->onConnectionChanged(true);
        c->onSnapshot(syntheticResetSnapshot(k.m50, k.m9, k.m14, k.faultCode));

        QVector<QPair<bool, QString>> terminals;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&terminals](Command cmd, bool ok, const QString &d) {
                    if (cmd == Command::Reset)
                        terminals.append({ok, d});
                });

        const ControlCoordinator::CommandResult r = c->reset();
        QVERIFY2(r.accepted, qPrintable(QStringLiteral("%1: reset rejected: %2")
                                            .arg(k.name, r.reason)));
        QVERIFY2(rec.pulses.contains(kM103),
                 qPrintable(QStringLiteral("%1: no M103 pulse was sent").arg(k.name)));

        // Deliver the correlated M103 pulse completion: the reset then converges
        // on its fixed boundary, and (user decision 2026-09-21) it never writes
        // the home-start coil, so the snapshot bits stay the only varying input.
        const ResetRecorder::Recorded pulse = rec.submissions.at(0);
        QCOMPARE(pulse.operation, PlcOperation::Pulse);
        QCOMPARE(pulse.address, kM103);
        c->onSubmissionCompleted(rec.completionFor(pulse, true));

        for (const ResetRecorder::Recorded &w : rec.submissions) {
            QVERIFY2(w.address != kM50,
                     qPrintable(QStringLiteral("%1: the reset wrote the home-start coil")
                                    .arg(k.name)));
        }

        now += ControlCoordinator::kResetCompletionDelayMs;
        c->onSnapshot(syntheticResetSnapshot(k.m50, k.m9, k.m14, k.faultCode));

        QCOMPARE(terminals.size(), 1);
        QVERIFY(terminals.first().first);
        QCOMPARE(terminals.first().second, QStringLiteral("复位完成"));
        results.append(terminals.first());
    }

    // Every scenario produced the identical single success: the result is
    // independent of M50, M61 (M9), M14 and D110.
    for (int i = 1; i < results.size(); ++i) {
        QCOMPARE(results[i].first, results[0].first);
        QCOMPARE(results[i].second, results[0].second);
    }
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
    gw.model().writeCoil(kM104, true);
    gw.tick();
    gw.model().writeCoil(kM101, true);
    gw.model().writeCoil(kM101, false);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3());

    QVERIFY(!c->reset().accepted); // M3=1: 禁止复位
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

    // D204 pinned to 1280 so the pulse-based run takes 7 s
    // (ceil(100 * 1280 / (15 * 1280))) and stays in progress after one tick.
    gw.model().writeRegister(kD204, 1280);
    QVERIFY(c->adjustWidth(300).accepted);
    QCOMPARE(gw.model().readRegister(kD128), quint16(300)); // D128 written
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34()); // adjusting
    QVERIFY(c->adjustInProgress());
    QCOMPARE(c->adjustTarget().value_or(0), quint16(300));
    // The saved start width/speed were consumed only by the removed
    // estimated-motion deadline (PLC-HMI-005 amendment 4); the result
    // comparison uses the target only.
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

    // D204 pinned to 1280: 7 s run (ceil(100*1280/(15*1280))), so the first
    // tick must not report success.
    gw.model().writeRegister(kD204, 1280);
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick(); // M34=1
    QVERIFY(!result); // no optimistic success

    // ceil(100 * 1280 / (15 * 1280)) = 7 s to complete.
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

    // Fixed T6 K300 timeout: 30 s (PLC-HMI-005 D2).
    for (int i = 0; i < 30; ++i)
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

    gw.model().writeRegister(kD204, 1280); // pin the 7 s run
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34());

    // M34=1: 禁止再次写 D128 或发送 M43 (spec §10.3 step 6).
    QVERIFY(!c->adjustWidth(350).accepted);
}

// A concurrent safety command (estop) must never leave the adjust flow hung in
// WaitTargetWrite: even though the D128 writeCompleted is dropped (its address
// no longer matches the overwritten pending write), the defensive write timeout
// converges the flow to a defined failure (spec §13: every phase converges).
void ControlCoordinatorTest::adjustWidthConcurrentEstopNeverHangs()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    ControlCoordinator::PulseTransport t;
    t.startPulse = [&gw](quint16 a) -> SubmissionResult {
        gw.model().writeCoil(a, true);
        gw.model().writeCoil(a, false);
        return acceptedResult();
    };
    t.writeHold = [&gw](quint16 a, bool v) -> SubmissionResult {
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) -> SubmissionResult {
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    // The D128 submission is accepted but its completion never arrives: the
    // adjust flow must still converge through its defensive result timeout.
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) -> SubmissionResult {
        if (a == kD128)
            return acceptedResult(); // deferred: completion never delivered
        gw.model().writeRegister(a, v);
        return acceptedResult();
    };

    std::unique_ptr<ControlCoordinator> c(makeCoordinatorWithCoil(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    int adjustReports = 0;
    bool writeResult = true;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::AdjustWidth) {
                    ++adjustReports;
                    writeResult = ok;
                    detail = d;
                }
            });

    // D128 write accepted and in flight: the flow sits in WaitTargetWrite.
    QVERIFY(c->adjustWidth(300).accepted);
    QVERIFY(c->adjustInProgress());

    // Concurrent estop overwrites the single-slot pending write.
    QVERIFY(c->estopSet().accepted);
    gw.tick(); // estop confirmed via M0/M100 readback
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m100());

    // Neither the D128 write nor the M43 pulse ever reports a correlated
    // completion here, so the flow has no evidence that the PLC saw the
    // command. The verdict (M34 + D130, user decision 2026-09-22) is therefore
    // not evaluated at all and convergence must come from the armed defensive
    // deadline — never a hang, never a double report.
    now += 30'000;
    gw.tick();
    QVERIFY(c->adjustInProgress()); // no verdict without a delivered pulse
    QCOMPARE(adjustReports, 0);

    now += 4'000; // past plc_timeout (30 s) + 3 s
    gw.tick();
    QVERIFY(!c->adjustInProgress());
    QCOMPARE(adjustReports, 1);
    QVERIFY(!writeResult);
    QVERIFY2(!detail.isEmpty(), "a terminal failure must carry a visible detail");

    gw.tick();
    QCOMPARE(adjustReports, 1); // exactly once
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
    gw.model().writeCoil(kM104, true);
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
    gw.model().writeCoil(kM104, true);
    gw.tick();
    gw.model().writeCoil(kM101, true);
    gw.model().writeCoil(kM101, false);
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

    gw.model().writeCoil(kM100, true);
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

// A release (M100=0) is in flight when a user issues an estop set (M100=1)
// before the release snapshot confirms. The newest command (set) must win:
// the stale release pending state is cleared so the release flow converges
// (reports failure/cancelled) instead of hanging on 待确认 forever, and the
// estop set still works. Spec §13: every flow converges to a defined result.
void ControlCoordinatorTest::estopSetDuringReleaseConverges()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // Estop is set (M0=1, M100=1).
    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m100());

    // The release flow must converge (report a result) rather than hang.
    bool releaseResult = false;
    bool releaseReported = false;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::EstopRelease) {
                    releaseResult = ok;
                    releaseReported = true;
                }
            });

    // Admin issues a release (M100=0 in flight, not yet snapshot-confirmed).
    QVERIFY(c->estopRelease().accepted);

    // Before the release snapshot confirms, a user issues an estop set. The
    // newest command wins: the stale release pending state must be cleared.
    QVERIFY(c->estopSet().accepted);

    // The estop set still works: M100=1 and M0=1.
    gw.tick();
    QVERIFY(gw.lastSnapshot().m100());
    QVERIFY(gw.lastSnapshot().m0());

    // The release flow converged (superseded by the newer set), so no 待确认
    // state persists.
    QVERIFY(releaseReported);
    QVERIFY(!releaseResult); // superseded: the release did not take effect
}

// Estop set: the M100 write is accepted but the snapshot never reflects
// M0/M100 -> the defensive timeout must converge to a defined failure instead
// of leaving the UI on 待确认 forever (spec §13).
void ControlCoordinatorTest::estopSetTimeoutConverges()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t;
    t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
        return acceptedResult(); // no-op write
    };
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorWithCoil(gw, now, t));
    c->setRole(Role::Anonymous);

    bool result = true;
    bool reported = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::EstopSet) {
                    result = ok;
                    reported = true;
                    detail = d;
                }
            });

    QVERIFY(c->estopSet().accepted);
    gw.tick(); // snapshot never shows M0/M100 (write was a no-op)
    QVERIFY(!reported); // still pending

    now += 5'001; // past kEstopTimeoutMs
    gw.tick();
    QVERIFY(reported);
    QVERIFY(!result); // converged to failure, not stuck
    QVERIFY(detail.contains(QStringLiteral("超时")));
}

// Estop release: same defensive timeout when the snapshot never reflects the
// M100=0 write (spec §13).
void ControlCoordinatorTest::estopReleaseTimeoutConverges()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t;
    t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
        return acceptedResult(); // no-op write
    };
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorWithCoil(gw, now, t));
    c->setRole(Role::Admin);

    // Estop is physically set (M0=1, M100=1) so the release cannot confirm
    // via the snapshot.
    gw.model().writeCoil(kM100, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m100());

    bool result = true;
    bool reported = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::EstopRelease) {
                    result = ok;
                    reported = true;
                    detail = d;
                }
            });

    QVERIFY(c->estopRelease().accepted);
    gw.tick(); // snapshot never shows M0=0/M100=0 (write was a no-op)
    QVERIFY(!reported); // still pending

    now += 5'001; // past kEstopTimeoutMs
    gw.tick();
    QVERIFY(reported);
    QVERIFY(!result); // converged to failure, not stuck
    QVERIFY(detail.contains(QStringLiteral("超时")));
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
    gw.model().writeCoil(kM100, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m14());
    QVERIFY(!c->manualHold(kM106, true).accepted); // press now rejected

    // Release must still be sent (write 0), not rejected by the interlocks.
    QVERIFY(c->manualHold(kM106, false).accepted);
    QVERIFY(!gw.model().readCoil(kM106));
}

// The release (write 0) must bypass machine-state interlocks but NOT the
// permission check: an anonymous/operator user must not be able to clear a
// held M106/M107/M108 (spec §10.7 只允许管理员, §11.4 所有写命令统一校验).
void ControlCoordinatorTest::manualHoldReleaseRequiresPermission()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    // Admin presses and holds M106.
    QVERIFY(c->manualHold(kM106, true).accepted);
    QVERIFY(gw.model().readCoil(kM106));

    // Log out: the anonymous user attempts to release the held M106.
    c->setRole(Role::Anonymous);
    QVERIFY(!c->manualHold(kM106, false).accepted);
    QVERIFY(gw.model().readCoil(kM106)); // write 0 NOT sent

    // Operator also cannot release.
    c->setRole(Role::Operator);
    QVERIFY(!c->manualHold(kM106, false).accepted);
    QVERIFY(gw.model().readCoil(kM106)); // write 0 NOT sent

    // Admin release still works even with a latched fault (round-3 behavior).
    c->setRole(Role::Admin);
    gw.model().writeCoil(kM100, true);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m14());
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
    t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
        return rejectedResult(QStringLiteral("transport rejected write"));
    };
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
    gw.model().writeCoil(kM100, true);
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
    gw.model().writeCoil(kM42, true);
    for (quint16 a = kM106; a <= kM111; ++a)
        gw.model().writeCoil(a, true);
    gw.model().writeCoil(kM100, true); // estop: must survive logout
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

    gw.model().writeCoil(kM105, true);
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

    // Stall: the PLC times out on its own after the fixed 30 s (T6 K300).
    // The HMI defensive timeout must not fire first, and the result must
    // converge to the actual M45 state.
    gw.model().setPositioningStall(true);
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(c->adjustInProgress());

    for (int i = 0; i < 30; ++i)
        gw.tick();
    QVERIFY(gw.lastSnapshot().m45());
    QVERIFY(!result); // converged to failure
    QVERIFY(!c->adjustInProgress());
}

// The defensive adjust deadline is the authoritative PLC timeout + 3 s
// (hmi_timeout = plc_timeout + 3, spec §10.3) with the fixed T6 K300 30 s
// width timeout (PLC-HMI-005 amendment 4): 33 s. A valid in-flight adjustment
// must never be misreported as timed out before the PLC's own 30 s result, and
// the deadline must still converge a flow whose pulse was lost (no PLC result
// will ever arrive).
void ControlCoordinatorTest::adjustDefensiveDeadlineIsPlcTimeoutPlusThreeSeconds()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinatorNoPulse(gw, now));
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

    // The M43 pulse never reaches the PLC (no-op transport), so no PLC result
    // can arrive: only the HMI defensive deadline converges the flow.
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(c->adjustInProgress());

    // 32 s: short of plc_timeout + 3 -> the deadline must not have fired.
    now += 32'000;
    gw.tick();
    QVERIFY(c->adjustInProgress());
    QVERIFY2(detail.isEmpty(), "no terminal detail before the 33 s deadline");

    // 33 s: the deadline fires exactly once with the terminal timeout failure.
    now += 1'000;
    gw.tick();
    QVERIFY(!c->adjustInProgress());
    QVERIFY(!result);
    QVERIFY(detail.contains(QStringLiteral("超时")));

    gw.tick();
    QVERIFY(!result); // exactly one terminal result, never re-reported
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
    gw.model().writeCoil(kM104, true);
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
    gw.model().writeCoil(kM104, true);
    gw.tick();
    gw.model().writeCoil(kM101, true);
    gw.model().writeCoil(kM101, false);
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
    t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
        return rejectedResult(QStringLiteral("transport rejected write"));
    };
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
    t.startPulse = [&gw](quint16 a) -> SubmissionResult {
        gw.model().writeCoil(a, true);
        gw.model().writeCoil(a, false);
        return acceptedResult();
    };
    t.writeHold = [&gw](quint16 a, bool v) -> SubmissionResult {
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) -> SubmissionResult {
        gw.model().writeRegister(a, v);
        return acceptedResult();
    };
    t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
        return acceptedResult(); // no-op select
    };
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

// --- 测试信号 M114-M117 (user decision 2026-09-22) -----------------------------

void ControlCoordinatorTest::simStationPulseIsAdminOnlyAndConvergesVisibly()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    gw.tick(); // publish a snapshot: the link is online

    // 仅管理员 (spec §11.4).
    c->setRole(Role::Operator);
    const ControlCoordinator::CommandResult denied = c->simStationPulse(114);
    QVERIFY(!denied.accepted);
    QVERIFY(denied.reason.contains(QStringLiteral("需要管理员权限")));

    c->setRole(Role::Admin);
    bool result = false;
    QString detail;
    int terminals = 0;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::SimUpstreamBoardIn) {
                    result = ok;
                    detail = d;
                    ++terminals;
                }
            });

    // One 100 ms pulse (1 -> 0) to M114, correlated by request identity.
    QVERIFY(c->simStationPulse(114).accepted);
    QVERIFY(!result); // no optimistic success before the correlated completion

    // A duplicate while the pulse is in flight is rejected visibly.
    const ControlCoordinator::CommandResult duplicate = c->simStationPulse(114);
    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());

    gw.tick(); // deliver the correlated pulse completion
    QVERIFY2(result, "the test pulse must converge to a visible success");
    QVERIFY(detail.contains(QStringLiteral("已发送")));
    QCOMPARE(terminals, 1); // exactly one terminal
    QVERIFY2(!gw.model().readCoil(114), "the pulse must end with the coil cleared");

    // The other three signals are independent identities and addresses.
    QVERIFY(c->simStationPulse(115).accepted);
    QVERIFY(c->simStationPulse(116).accepted);
    QVERIFY(c->simStationPulse(117).accepted);
    gw.tick();

    // An address outside the four defined signals is reported, never swallowed.
    const ControlCoordinator::CommandResult unknown = c->simStationPulse(113);
    QVERIFY(!unknown.accepted);
    QVERIFY(!unknown.reason.isEmpty());
}

QTEST_GUILESS_MAIN(ControlCoordinatorTest)
#include "test_control_coordinator.moc"
