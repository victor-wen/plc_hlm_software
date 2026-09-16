// PLC-HMI-001 black-box tests: operator-command lifecycle at the
// ControlCoordinator boundary (brief OB-1, OB-2, OB-3, OB-6, OB-7, OB-11).
//
// Authored from the behavior-only brief .ai/test-briefs/PLC-HMI-001.yaml and
// the approved .ai/project-contract.yaml. No production implementation source
// was consulted.
//
// PLC-HMI-003 revision: the coordinator seam now returns SubmissionResult from
// every PulseTransport callback and reports terminal completions through
// onSubmissionCompleted(); this file was migrated to that revised seam without
// changing any assertion intent. The SimulatedPlcGateway is used only for
// snapshots and connection state, so the target fails to compile until the
// revised API exists (expected PLC-HMI-003 RED).

#include <QtTest>
#include <QSignalSpy>

#include <functional>
#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "application/control_coordinator.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM100 = 100;
constexpr quint16 kM101 = 101;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kM109 = 109;
constexpr quint16 kM110 = 110;
constexpr quint16 kD128 = 128;
constexpr quint16 kD204 = 204; // pulse per mm (adjust-run timing fixture)

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

ControlCoordinator::PulseTransport gatewayTransport(SimulatedPlcGateway &gw)
{
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
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) -> SubmissionResult {
        gw.model().writeRegister(a, v);
        return acceptedResult();
    };
    return t;
}

ControlCoordinator *wire(SimulatedPlcGateway &gw, qint64 &now,
                         ControlCoordinator::PulseTransport t,
                         ControlCoordinator::Config cfg = {})
{
    auto *c = new ControlCoordinator(t, cfg, [&now]() { return now; });
    QObject::connect(&gw, &SimulatedPlcGateway::snapshotReady, c,
                     [c](quint64, const DeviceSnapshot &s) { c->onSnapshot(s); });
    QObject::connect(&gw, &SimulatedPlcGateway::connectionStateChanged, c,
                     [c](quint64, bool online) { c->onConnectionChanged(online); });
    if (gw.hasSnapshot())
        c->onSnapshot(gw.lastSnapshot());
    return c;
}

ControlCoordinator *makeCoordinator(SimulatedPlcGateway &gw, qint64 &now,
                                    ControlCoordinator::Config cfg = {})
{
    return wire(gw, now, gatewayTransport(gw), cfg);
}

// The M101/M102 pulse is accepted by the transport but never reaches the PLC,
// so the flow stays pending: used for timeout convergence tests.
ControlCoordinator *makeNoPulseCoordinator(SimulatedPlcGateway &gw, qint64 &now,
                                           ControlCoordinator::Config cfg = {})
{
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.startPulse = [](quint16) { return acceptedResult(); };
    return wire(gw, now, t, cfg);
}

struct ResultRecord
{
    bool ok = true;
    QString detail;
};

} // namespace

class OperatorCommandLifecycleTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-2: duplicate / in-progress requests are visibly rejected ---------
    void duplicateResetRejectedVisibly();
    void duplicateStartRejectedVisibly();
    void duplicateStopRejectedVisibly();
    void duplicateAdjustRejectedVisibly();
    void duplicateModeSwitchRejectedVisibly();
    void overlappingResetAndModeSwitchNeverSilent();
    void overlappingModeSwitchThenResetNeverSilent();
    void duplicateManualHoldWhilePendingVisiblyRejected();

    // --- OB-3: accepted commands converge to exactly one terminal result -----
    void startTimeoutConvergesToExactlyOneFailure();
    void stopTimeoutConvergesToExactlyOneFailure();
    void modeSwitchTimeoutConvergesToExactlyOneFailure();
    void linkLossConvergesResetToTerminal();
    void linkLossConvergesStartToTerminal();
    void linkLossConvergesStopToTerminal();
    void linkLossConvergesModeSwitchToTerminal();
    void linkLossConvergesAdjustToTerminal();
    void linkLossConvergesEstopSetToTerminal();
    void linkLossConvergesEstopReleaseToTerminal();
    void adjustTimeoutConvergesToExactlyOneTerminal();
    void linkLossConvergesManualHoldToTerminal();
    void linkLossConvergesManualLatchToTerminal();
    void linkLossConvergesBypassToTerminal();

    // --- OB-6: no optimistic success for manual hold / latch / bypass --------
    void manualHoldNotSuccessfulBeforeConfirmation();
    void manualHoldTransportRejectionIsVisible();
    void manualHoldNoConfirmationConvergesToFailure();
    void manualLatchNoConfirmationConvergesToFailure();
    void bypassNoConfirmationConvergesToFailure();
    void manualLatchNotSuccessfulBeforeConfirmation();
    void bypassNotSuccessfulBeforeConfirmation();
    void bypassTransportRejectionIsVisible();

    // --- OB-7: software-estop release wording distinguishes physical estop ---
    void estopReleaseNamesPhysicalEstopWhenM0StillHeld();
    void estopReleaseConvergesWhenPhysicalEstopCleared();

    // --- OB-11: permission and interlock behavior unchanged ------------------
    void anonymousAdminOnlyCommandsRejectedWithReasonsAndNoWrites();
    void operatorRoleAdminOnlyCommandsRejectedWithReasons();
    void interlockRejectionsStillCarryVisibleReasons();
};

// --- OB-2 ---------------------------------------------------------------------

void OperatorCommandLifecycleTest::duplicateResetRejectedVisibly()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->reset().accepted);
    QVERIFY(c->resetInProgress());

    const ControlCoordinator::CommandResult duplicate = c->reset();
    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());

    // The duplicate must be observable on the signal surface, not only as a
    // return value (brief OB-2 / error_behaviors).
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::Reset);
    QVERIFY(!rejected[0][1].toString().isEmpty());
}

void OperatorCommandLifecycleTest::duplicateStartRejectedVisibly()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeNoPulseCoordinator(gw, now));
    c->setRole(Role::Operator);
    homeReady(gw);
    putInAutoMode(gw);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->start().accepted);
    QVERIFY(c->startInProgress());

    const ControlCoordinator::CommandResult duplicate = c->start();
    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::Start);
    QVERIFY(!rejected[0][1].toString().isEmpty());
}

void OperatorCommandLifecycleTest::duplicateStopRejectedVisibly()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeNoPulseCoordinator(gw, now));
    c->setRole(Role::Anonymous);
    homeReady(gw);
    putInAutoMode(gw);

    // Machine running: M3=1 via the raw gateway.
    gw.model().writeCoil(kM101, true);
    gw.model().writeCoil(kM101, false);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3());

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->stop().accepted);
    QVERIFY(c->stopInProgress());

    const ControlCoordinator::CommandResult duplicate = c->stop();
    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::Stop);
    QVERIFY(!rejected[0][1].toString().isEmpty());
}

void OperatorCommandLifecycleTest::duplicateAdjustRejectedVisibly()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    // PLC-HMI-005 parity supersession: with the corrected authoritative D204
    // default (128) this 100 mm adjust completes almost immediately, leaving no
    // in-flight adjust to duplicate. Pin the previously assumed pulse density
    // (1280 pulses/mm, a valid decoded D204) through the public model seam so
    // the run spans several seconds and the duplicate rejection is still
    // exercised while the adjust is genuinely in progress. No assertion below
    // is changed.
    gw.model().writeRegister(kD204, 1280);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick(); // M34=1: adjusting
    QVERIFY(c->adjustInProgress());
    QVERIFY(gw.lastSnapshot().m34());

    const ControlCoordinator::CommandResult duplicate = c->adjustWidth(350);
    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::AdjustWidth);
    QVERIFY(!rejected[0][1].toString().isEmpty());
}

void OperatorCommandLifecycleTest::duplicateModeSwitchRejectedVisibly()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    // The M104 select write is accepted by the transport but never reaches the
    // PLC, so the mode switch stays pending.
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [](quint16, bool, CommandPriority) { return acceptedResult(); };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->setMode(true).accepted);

    const ControlCoordinator::CommandResult duplicate = c->setMode(false);
    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::ModeSwitch);
    QVERIFY(!rejected[0][1].toString().isEmpty());
}

void OperatorCommandLifecycleTest::overlappingResetAndModeSwitchNeverSilent()
{
    // Reset and mode switch both write M104. The second command must never
    // return without any visible signal: it either converges or is visibly
    // rejected (brief boundary_cases).
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);
    putInAutoMode(gw);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
    QSignalSpy accepted(c.get(), &ControlCoordinator::commandAccepted);

    QVERIFY(c->reset().accepted); // auto -> manual + M103 pending
    QVERIFY(c->resetInProgress());

    const ControlCoordinator::CommandResult second = c->setMode(true);
    const bool visible = rejected.count() > 0 || accepted.count() >= 2
        || !second.accepted;
    QVERIFY2(visible, "overlapping M104 command was silent");
    if (rejected.count() > 0)
        QVERIFY(!rejected[0][1].toString().isEmpty());
}

void OperatorCommandLifecycleTest::overlappingModeSwitchThenResetNeverSilent()
{
    // Reverse order of the overlap above: a mode switch is pending and a reset
    // arrives second. The second request must never be silent (brief
    // boundary_cases: reset while a mode switch is pending and vice versa).
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [](quint16, bool, CommandPriority) { return acceptedResult(); };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
    QSignalSpy accepted(c.get(), &ControlCoordinator::commandAccepted);
    QSignalSpy pending(c.get(), &ControlCoordinator::commandPending);

    QVERIFY(c->setMode(true).accepted); // pending: M104 select never applied

    const int rejectedBefore = rejected.count();
    const int acceptedBefore = accepted.count();
    const int pendingBefore = pending.count();

    const ControlCoordinator::CommandResult second = c->reset();

    // The second request itself must produce a visible signal: rejected,
    // accepted, or pending; it may never return silently.
    const bool visible = !second.accepted
        || rejected.count() > rejectedBefore
        || accepted.count() > acceptedBefore
        || pending.count() > pendingBefore;
    QVERIFY2(visible, "overlapping M104 reset was silent");
    if (!second.accepted)
        QVERIFY(!second.reason.isEmpty());
    if (rejected.count() > rejectedBefore)
        QVERIFY(!rejected[rejectedBefore][1].toString().isEmpty());
}

void OperatorCommandLifecycleTest::duplicateManualHoldWhilePendingVisiblyRejected()
{
    // Brief OB-2: a duplicate/in-progress request for any machine command emits
    // a visible rejection, so the manual family must not return silently.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [](quint16, bool) { return acceptedResult(); }; // accepted, not applied
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->manualHold(kM106, true).accepted);

    const ControlCoordinator::CommandResult duplicate = c->manualHold(kM106, true);
    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QVERIFY(!rejected[0][1].toString().isEmpty());
}

// --- OB-3 ---------------------------------------------------------------------

void OperatorCommandLifecycleTest::startTimeoutConvergesToExactlyOneFailure()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeNoPulseCoordinator(gw, now));
    c->setRole(Role::Operator);
    homeReady(gw);
    putInAutoMode(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::Start)
                    results.append({ok, detail});
            });

    QVERIFY(c->start().accepted);
    gw.tick();
    QVERIFY(c->startInProgress());

    now += 10'001; // past the start/stop defensive timeout
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
    QVERIFY(!c->startInProgress());

    // Exactly one terminal state: later snapshots must not emit another result.
    gw.tick();
    now += 10'001;
    gw.tick();
    QCOMPARE(results.size(), 1);
}

void OperatorCommandLifecycleTest::stopTimeoutConvergesToExactlyOneFailure()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeNoPulseCoordinator(gw, now));
    c->setRole(Role::Anonymous);
    homeReady(gw);
    putInAutoMode(gw);

    gw.model().writeCoil(kM101, true);
    gw.model().writeCoil(kM101, false);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3());

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::Stop)
                    results.append({ok, detail});
            });

    QVERIFY(c->stop().accepted);
    gw.tick();
    QVERIFY(c->stopInProgress());

    now += 10'001;
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
    QVERIFY(!c->stopInProgress());

    gw.tick();
    QCOMPARE(results.size(), 1);
}

void OperatorCommandLifecycleTest::modeSwitchTimeoutConvergesToExactlyOneFailure()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [](quint16, bool, CommandPriority) { return acceptedResult(); };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::ModeSwitch)
                    results.append({ok, detail});
            });

    QVERIFY(c->setMode(true).accepted);
    QVERIFY(results.isEmpty()); // no optimistic success

    now += 5'001;
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());

    gw.tick();
    now += 5'001;
    gw.tick();
    QCOMPARE(results.size(), 1);
}

void OperatorCommandLifecycleTest::linkLossConvergesResetToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::Reset)
                    results.append({ok, detail});
            });

    QVERIFY(c->reset().accepted);
    gw.tick(); // homing in progress

    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
    QVERIFY(!c->resetInProgress());
}

void OperatorCommandLifecycleTest::linkLossConvergesStartToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeNoPulseCoordinator(gw, now));
    c->setRole(Role::Operator);
    homeReady(gw);
    putInAutoMode(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::Start)
                    results.append({ok, detail});
            });

    QVERIFY(c->start().accepted);
    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
    QVERIFY(!c->startInProgress());
}

void OperatorCommandLifecycleTest::linkLossConvergesStopToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeNoPulseCoordinator(gw, now));
    c->setRole(Role::Anonymous);

    gw.model().writeCoil(kM101, true);
    gw.model().writeCoil(kM101, false);
    gw.tick();

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::Stop)
                    results.append({ok, detail});
            });

    QVERIFY(c->stop().accepted);
    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
    QVERIFY(!c->stopInProgress());
}

void OperatorCommandLifecycleTest::linkLossConvergesModeSwitchToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [](quint16, bool, CommandPriority) { return acceptedResult(); };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::ModeSwitch)
                    results.append({ok, detail});
            });

    QVERIFY(c->setMode(true).accepted);
    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::linkLossConvergesAdjustToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    // PLC-HMI-005 parity supersession: with the corrected authoritative D204
    // default (128) this 100 mm adjust completes almost immediately, so the
    // link loss landed after a success instead of converging a pending adjust.
    // Pin the previously assumed pulse density (1280 pulses/mm, a valid decoded
    // D204) through the public model seam so the adjust is still in flight when
    // the link loss is injected. No assertion below is changed.
    gw.model().writeRegister(kD204, 1280);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::AdjustWidth)
                    results.append({ok, detail});
            });

    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick(); // M34=1: adjusting

    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
    QVERIFY(!c->adjustInProgress());
}

void OperatorCommandLifecycleTest::linkLossConvergesEstopSetToTerminal()
{
    // Brief error_behaviors: link loss before confirmation converges the
    // command to a visible communications-lost terminal state; it must not
    // stay pending forever.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Anonymous);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::EstopSet)
                    results.append({ok, detail});
            });

    QVERIFY(c->estopSet().accepted);
    QVERIFY(results.isEmpty()); // not confirmed yet, not optimistic

    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::linkLossConvergesEstopReleaseToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // Software estop is set and confirmed via the snapshot.
    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m100());

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::EstopRelease)
                    results.append({ok, detail});
            });

    QVERIFY(c->estopRelease().accepted);
    QVERIFY(results.isEmpty()); // not confirmed yet

    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::adjustTimeoutConvergesToExactlyOneTerminal()
{
    // Brief boundary_cases: adjust timeout. The D128 write is accepted but the
    // M43 pulse never reaches the PLC, so the command stays pending until the
    // defensive timeout with the injected clock converges it.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeNoPulseCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::AdjustWidth)
                    results.append({ok, detail});
            });

    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(results.isEmpty()); // no optimistic success

    now += 3'600'000;
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
    QVERIFY(!c->adjustInProgress());

    gw.tick();
    QCOMPARE(results.size(), 1);
}

void OperatorCommandLifecycleTest::linkLossConvergesManualHoldToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [](quint16, bool) { return acceptedResult(); }; // accepted, not applied
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->manualHold(kM106, true).accepted);
    QVERIFY(results.isEmpty());

    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::linkLossConvergesManualLatchToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [&gw](quint16 a, bool v) {
        if (a == kM109)
            return acceptedResult(); // accepted, not applied
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority p) {
        if (a == kM109)
            return acceptedResult();
        return gatewayTransport(gw).writeCoil(a, v, p);
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->manualLatch(kM109, true).accepted);
    QVERIFY(results.isEmpty());

    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::linkLossConvergesBypassToTerminal()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) {
        if (a == kM110)
            return acceptedResult(); // accepted, not applied
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->bypass(kM110, true).accepted);
    QVERIFY(results.isEmpty());

    gw.setLinkDown(true);
    c->onConnectionChanged(false);
    gw.tick();

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

// --- OB-6 ---------------------------------------------------------------------

void OperatorCommandLifecycleTest::manualHoldNotSuccessfulBeforeConfirmation()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    // The hold write is accepted by the transport but does not change the PLC
    // state until the test writes it directly.
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [](quint16, bool) { return acceptedResult(); };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->manualHold(kM106, true).accepted);

    // No PLC confirmation yet: success must not be reported.
    QVERIFY2(results.isEmpty(),
             qPrintable(QStringLiteral("manual hold reported %1 result(s) "
                                       "before confirmation: %2")
                            .arg(results.size())
                            .arg(results.isEmpty() ? QString()
                                                   : results[0].detail)));
    gw.tick();
    QVERIFY(results.isEmpty());

    // Confirmed snapshot shows the requested state: success is now visible.
    gw.model().writeCoil(kM106, true);
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());

    // Exactly one terminal result for the accepted command.
    gw.tick();
    QCOMPARE(results.size(), 1);
}

void OperatorCommandLifecycleTest::manualHoldTransportRejectionIsVisible()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    // The transport rejects the submission with an immediate reason.
    t.writeHold = [](quint16, bool) {
        return rejectedResult(QStringLiteral("transport rejected the hold write"));
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult r = c->manualHold(kM106, true);
    if (r.accepted) {
        // Accepted: the rejected transport write must surface as a failure,
        // not silence and not success.
        QCOMPARE(results.size(), 1);
        QVERIFY(!results[0].ok);
        QVERIFY(!results[0].detail.isEmpty());
    } else {
        // Rejected synchronously: the rejection must carry a reason and be
        // observable on the signal surface, never silent.
        QVERIFY(!r.reason.isEmpty());
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!rejected[0][1].toString().isEmpty());
    }
}

void OperatorCommandLifecycleTest::manualHoldNoConfirmationConvergesToFailure()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [](quint16, bool) { return acceptedResult(); }; // accepted but lost
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->manualHold(kM106, true).accepted);
    gw.tick();
    QVERIFY(results.isEmpty()); // still waiting for confirmation

    // Defensive timeout with the injected clock converges the command.
    now += 3'600'000;
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::manualLatchNoConfirmationConvergesToFailure()
{
    // Brief error_behaviors: a pending manual command with no confirmation
    // converges via a bounded defensive timeout with a non-empty detail.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [&gw](quint16 a, bool v) {
        if (a == kM109)
            return acceptedResult(); // accepted, not applied
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority p) {
        if (a == kM109)
            return acceptedResult();
        return gatewayTransport(gw).writeCoil(a, v, p);
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->manualLatch(kM109, true).accepted);
    gw.tick();
    QVERIFY(results.isEmpty()); // still waiting for confirmation

    now += 3'600'000;
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::bypassNoConfirmationConvergesToFailure()
{
    // Brief error_behaviors: a pending bypass command with no confirmation
    // converges via a bounded defensive timeout with a non-empty detail.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) {
        if (a == kM110)
            return acceptedResult(); // accepted, not applied
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->bypass(kM110, true).accepted);
    gw.tick();
    QVERIFY(results.isEmpty()); // still waiting for confirmation

    now += 3'600'000;
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::manualLatchNotSuccessfulBeforeConfirmation()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [&gw](quint16 a, bool v) {
        if (a == kM109)
            return acceptedResult(); // accepted but not applied
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority p) {
        if (a == kM109)
            return acceptedResult();
        return gatewayTransport(gw).writeCoil(a, v, p);
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->manualLatch(kM109, true).accepted);
    QVERIFY2(results.isEmpty(), "manual latch reported success before the PLC "
                                "confirmed the requested latch state");

    gw.model().writeCoil(kM109, true);
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::bypassNotSuccessfulBeforeConfirmation()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) {
        if (a == kM110)
            return acceptedResult(); // accepted but not applied
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });

    QVERIFY(c->bypass(kM110, true).accepted);
    QVERIFY2(results.isEmpty(), "bypass reported success before the PLC "
                                "confirmed the requested state");

    gw.model().writeCoil(kM110, true);
    gw.tick();
    QCOMPARE(results.size(), 1);
    QVERIFY(results[0].ok);
    QVERIFY(!results[0].detail.isEmpty());
}

void OperatorCommandLifecycleTest::bypassTransportRejectionIsVisible()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;

    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeCoil = [](quint16, bool, CommandPriority) {
        return rejectedResult(QStringLiteral("transport rejected the bypass write"));
    };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command, bool ok, const QString &detail) {
                results.append({ok, detail});
            });
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult r = c->bypass(kM110, true);
    if (r.accepted) {
        QCOMPARE(results.size(), 1);
        QVERIFY(!results[0].ok);
        QVERIFY(!results[0].detail.isEmpty());
    } else {
        QVERIFY(!r.reason.isEmpty());
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!rejected[0][1].toString().isEmpty());
    }
}

// --- OB-7 ---------------------------------------------------------------------

void OperatorCommandLifecycleTest::estopReleaseNamesPhysicalEstopWhenM0StillHeld()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m100());

    // Physical estop keeps M0=1 after the HMI clears M100.
    gw.model().setEstopReleaseStuck(true);

    bool reported = false;
    bool ok = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool resultOk, const QString &resultDetail) {
                if (cmd == Command::EstopRelease) {
                    reported = true;
                    ok = resultOk;
                    detail = resultDetail;
                }
            });

    QVERIFY(c->estopRelease().accepted);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m100()); // software request released
    QVERIFY(gw.lastSnapshot().m0());    // physical estop still holds

    QVERIFY(reported);
    QVERIFY(ok); // the software-estop request release itself converged
    QVERIFY2(detail.contains(QStringLiteral("实体急停")),
             qPrintable(QStringLiteral("release detail must name the physical "
                                       "estop, got: %1").arg(detail)));
    QVERIFY2(!detail.contains(QStringLiteral("急停已解除")),
             qPrintable(QStringLiteral("release detail must not claim the "
                                       "machine is fully released, got: %1")
                            .arg(detail)));
}

void OperatorCommandLifecycleTest::estopReleaseConvergesWhenPhysicalEstopCleared()
{
    // Brief OB-7 / boundary_cases: estop release when M100 clears and M0 also
    // clears may be reported as complete; the command still converges to
    // exactly one terminal result with a non-empty detail.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m100());

    bool reported = false;
    bool ok = false;
    QString detail;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool resultOk, const QString &resultDetail) {
                if (cmd == Command::EstopRelease) {
                    reported = true;
                    ok = resultOk;
                    detail = resultDetail;
                }
            });

    QVERIFY(c->estopRelease().accepted);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m100()); // software request released
    QVERIFY(!gw.lastSnapshot().m0());   // physical estop cleared

    QVERIFY(reported);
    QVERIFY(ok); // both M100 and M0 cleared: release is complete
    QVERIFY(!detail.isEmpty());
}

// --- OB-11 --------------------------------------------------------------------

void OperatorCommandLifecycleTest::anonymousAdminOnlyCommandsRejectedWithReasonsAndNoWrites()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Anonymous);
    homeReady(gw);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(!c->reset().accepted);
    QVERIFY(!c->adjustWidth(300).accepted);
    QVERIFY(!c->setMode(true).accepted);
    QVERIFY(!c->estopRelease().accepted);
    QVERIFY(!c->manualHold(kM106, true).accepted);
    QVERIFY(!c->manualLatch(kM109, true).accepted);
    QVERIFY(!c->bypass(kM110, true).accepted);

    QCOMPARE(rejected.count(), 7);
    for (int i = 0; i < rejected.count(); ++i) {
        QVERIFY2(!rejected[i][1].toString().isEmpty(),
                 qPrintable(QStringLiteral("rejection %1 has an empty reason")
                                .arg(i)));
    }

    // No administrator-only write may have been applied.
    QVERIFY(!gw.model().readCoil(kM104));
    QVERIFY(!gw.model().readCoil(kM103));
    QVERIFY(!gw.model().readCoil(kM106));
    QVERIFY(!gw.model().readCoil(kM109));
    QVERIFY(!gw.model().readCoil(kM110));

    // Stop and software-estop set stay available to anonymous users.
    QVERIFY(c->stop().accepted);
    QVERIFY(c->estopSet().accepted);
}

void OperatorCommandLifecycleTest::operatorRoleAdminOnlyCommandsRejectedWithReasons()
{
    // Brief OB-11: anonymous/operator roles still receive visible rejections
    // with reasons for administrator-only commands (matrix unchanged).
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Operator);
    homeReady(gw);

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(!c->reset().accepted);
    QVERIFY(!c->adjustWidth(300).accepted);
    QVERIFY(!c->setMode(true).accepted);
    QVERIFY(!c->estopRelease().accepted);
    QVERIFY(!c->manualHold(kM106, true).accepted);
    QVERIFY(!c->manualLatch(kM109, true).accepted);
    QVERIFY(!c->bypass(kM110, true).accepted);

    QCOMPARE(rejected.count(), 7);
    for (int i = 0; i < rejected.count(); ++i) {
        QVERIFY2(!rejected[i][1].toString().isEmpty(),
                 qPrintable(QStringLiteral("operator-role rejection %1 has an "
                                           "empty reason")
                                .arg(i)));
    }

    // No administrator-only write may have been applied.
    QVERIFY(!gw.model().readCoil(kM104));
    QVERIFY(!gw.model().readCoil(kM103));
    QVERIFY(!gw.model().readCoil(kM106));
    QVERIFY(!gw.model().readCoil(kM109));
    QVERIFY(!gw.model().readCoil(kM110));

    // Stop and software-estop set stay available to operators.
    QVERIFY(c->stop().accepted);
    QVERIFY(c->estopSet().accepted);
}

void OperatorCommandLifecycleTest::interlockRejectionsStillCarryVisibleReasons()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Operator);

    // Not homed / not auto: start must be visibly rejected with a reason and no
    // M101 pulse may be sent.
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
    const ControlCoordinator::CommandResult start = c->start();
    QVERIFY(!start.accepted);
    QVERIFY(!start.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::Start);
    QVERIFY(!rejected[0][1].toString().isEmpty());
    QVERIFY(!gw.model().readCoil(kM101));

    // Not homed: adjust must be visibly rejected and must not write D128.
    const ControlCoordinator::CommandResult adjust = c->adjustWidth(300);
    QVERIFY(!adjust.accepted);
    QVERIFY(!adjust.reason.isEmpty());
    QCOMPARE(rejected.count(), 2);
    QVERIFY(!gw.model().readCoil(43));
    QCOMPARE(gw.model().readRegister(kD128), quint16(200)); // unchanged default
}

QTEST_GUILESS_MAIN(OperatorCommandLifecycleTest)
#include "operator_command_lifecycle_test.moc"
