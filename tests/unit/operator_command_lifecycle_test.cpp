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

#include "adapters/simulator/h3u_simulation_model.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/simulator/simulation_clock.h"
#include "application/control_coordinator.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM100 = 100;
constexpr quint16 kM101 = 101;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kM107 = 107;
constexpr quint16 kM108 = 108;
constexpr quint16 kM109 = 109;
constexpr quint16 kM110 = 110;
constexpr quint16 kD128 = 128;
constexpr quint16 kD204 = 204; // pulse per mm (adjust-run timing fixture)

// PLC-HMI-011: the home-start coil (M50), written once after the reset pulse,
// and the home-complete mirror (M61 -> M9 in statusWord1).
constexpr quint16 kM50 = 50;
constexpr quint16 kM61 = 61;

// PLC-HMI-011 simulator-handshake fixture addresses.
constexpr quint16 kM0 = 0;   // physical/software estop state
constexpr quint16 kM14 = 14; // latched fault
constexpr quint16 kM34 = 34; // width adjustment in progress
constexpr quint16 kM43 = 43; // width-adjust pulse
constexpr quint16 kM44 = 44; // width-adjust success
constexpr quint16 kM45 = 45; // width-adjust failure
constexpr quint16 kD110 = 110; // fault code
constexpr quint16 kD220 = 220; // width speed

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
    // The decoded M43 preconditions require manual mode M1.
    gw.model().writeCoil(kM104, false);
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.model().writeCoil(kM50, true); // PLC-HMI-011: homing starts on the home-start write
    gw.tick();
    gw.tick(); // home return takes 2 s
    // User decision 2026-09-22: the decoded SBR_HOME completion zeroes D130
    // (`DMOV K0 D130`), and the decoded M60 rung is
    // M61 AND D128==D130 AND NOT M0 AND NOT M14 AND NOT T6 — so a homed machine
    // is not 自动准备完成 until one width adjust actually reaches the target.
    // With the model defaults (D128=200, D204=128, D220=15) that run takes 2 s.
    gw.model().writeCoil(kM43, true);
    gw.model().writeCoil(kM43, false);
    gw.tick();
    gw.tick();
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

// --- PLC-HMI-010 reset fixture helpers ----------------------------------------
//
// The reset request is a fire-and-confirm-by-fixed-delay command: after an
// accepted administrator reset on a not-running machine it must converge to a
// single success terminal carrying exactly 复位完成 once a fixed 200 ms has
// elapsed (brief OB-1), independently of the controller's completion/fault
// bits (brief OB-2). Timing is driven through the injected clock and synthetic
// snapshots (the established coordinator seam), so no simulator detail is
// needed and the reset signal itself is observed on a recording transport.

constexpr qint64 kResetFixedDelayMs = 200;

struct ResetObservation
{
    QVector<ResultRecord> results;
    int pendingStates = 0; // commandPending(Reset) emissions

    int terminalCount() const { return results.size(); }

    int successCount() const
    {
        int count = 0;
        for (const ResultRecord &r : results) {
            if (r.ok)
                ++count;
        }
        return count;
    }
};

void observeReset(ControlCoordinator &c, ResetObservation &obs, QObject *context)
{
    QObject::connect(&c, &ControlCoordinator::commandResult, context,
                     [&obs](Command cmd, bool ok, const QString &detail) {
                         if (cmd == Command::Reset)
                             obs.results.append({ok, detail});
                     });
    QObject::connect(&c, &ControlCoordinator::commandPending, context,
                     [&obs](Command cmd) {
                         if (cmd == Command::Reset)
                             ++obs.pendingStates;
                     });
}

QString describeResetResults(const QVector<ResultRecord> &results)
{
    QStringList parts;
    int index = 0;
    for (const ResultRecord &r : results) {
        parts.append(QStringLiteral("#%1 ok=%2 detail='%3'")
                         .arg(index++)
                         .arg(r.ok ? QStringLiteral("true") : QStringLiteral("false"))
                         .arg(r.detail));
    }
    return parts.join(QStringLiteral(" | "));
}

// Records every submission the coordinator makes and answers with a correlated
// accepted/rejected SubmissionResult. No PLC state is mutated: the machine
// state is supplied as synthetic snapshots, which keeps OB-2 independent of the
// simulator's own flag handling.
//
// PLC-HMI-011 extension: the recorder now also records the write kind and the
// submission order, and can deliver the correlated terminal completion for a
// chosen submission on demand, so a test can control whether and when the
// home-start write (M50) completes. This is a transport reply, not production
// logic: the test acts as the controller that answers a request identity.
struct ResetTransportRecorder
{
    enum class WriteKind { Pulse, Hold, Coil, Register };

    struct WriteRecord
    {
        WriteKind kind = WriteKind::Coil;
        quint16 address = 0;
        quint16 value = 0;
        quint64 requestId = 0;
    };

    QVector<quint16> pulses;
    QVector<QPair<quint16, bool>> coils;
    QVector<QPair<quint16, bool>> holds;
    QVector<QPair<quint16, quint16>> registers;
    QVector<WriteRecord> writes; // every submission, in submission order

    bool rejectPulses = false;
    QString pulseRejectReason;

    // --- PLC-HMI-011: per-submission outcome control --------------------------
    bool rejectHomeStart = false;
    QString homeStartRejectReason;

    ControlCoordinator::PulseTransport make()
    {
        ControlCoordinator::PulseTransport t;
        t.startPulse = [this](quint16 address) -> SubmissionResult {
            if (rejectPulses) {
                SubmissionResult r;
                r.accepted = false;
                r.gateway_generation = kGeneration;
                r.immediate_rejection_reason = pulseRejectReason;
                return r;
            }
            pulses.append(address);
            const SubmissionResult r = accept();
            writes.append({WriteKind::Pulse, address, true, r.request_id});
            return r;
        };
        t.writeHold = [this](quint16 address, bool value) -> SubmissionResult {
            holds.append({address, value});
            const SubmissionResult r = accept();
            writes.append({WriteKind::Hold, address, value, r.request_id});
            return r;
        };
        t.writeCoil = [this](quint16 address, bool value, CommandPriority) -> SubmissionResult {
            if (address == kM50 && rejectHomeStart) {
                SubmissionResult r;
                r.accepted = false;
                r.gateway_generation = kGeneration;
                r.immediate_rejection_reason = homeStartRejectReason;
                return r;
            }
            coils.append({address, value});
            const SubmissionResult r = accept();
            writes.append({WriteKind::Coil, address, value, r.request_id});
            return r;
        };
        t.writeRegister = [this](quint16 address, quint16 value, CommandPriority)
            -> SubmissionResult {
            registers.append({address, value});
            const SubmissionResult r = accept();
            writes.append({WriteKind::Register, address, value, r.request_id});
            return r;
        };
        return t;
    }

    // --- PLC-HMI-011: completion control --------------------------------------

    // Every accepted submission whose address is `address` and whose coil value
    // is `value`, oldest first.
    QVector<WriteRecord> writesTo(quint16 address, bool value) const
    {
        QVector<WriteRecord> matches;
        for (const WriteRecord &w : writes) {
            if (w.address == address && w.value == value)
                matches.append(w);
        }
        return matches;
    }

    int writeCount(quint16 address, bool value) const
    {
        return writesTo(address, value).size();
    }

    bool hasWriteTo(quint16 address, bool value) const
    {
        return writeCount(address, value) > 0;
    }

    // The first write of `value` to `address`, or a zero record when absent.
    WriteRecord firstWriteTo(quint16 address, bool value) const
    {
        const QVector<WriteRecord> matches = writesTo(address, value);
        return matches.isEmpty() ? WriteRecord{} : matches.first();
    }

    // Delivers the correlated terminal completion for a recorded submission, as
    // the controller would. An unknown request id is a test error and returns
    // false without delivering anything.
    bool complete(ControlCoordinator &coordinator, quint64 requestId, bool result,
                  const QString &error = QString())
    {
        WriteRecord record;
        bool found = false;
        for (const WriteRecord &w : writes) {
            if (w.requestId == requestId) {
                record = w;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        SubmissionCompletion completion;
        completion.request_id = requestId;
        completion.gateway_generation = kGeneration;
        completion.operation = operationOf(record);
        completion.address = record.address;
        completion.result = result;
        completion.error = error;
        coordinator.onSubmissionCompleted(completion);
        return true;
    }

    static constexpr quint64 kGeneration = 1;

private:
    SubmissionResult accept()
    {
        SubmissionResult r;
        r.accepted = true;
        r.request_id = ++m_nextId;
        r.gateway_generation = kGeneration;
        return r;
    }

    static PlcOperation operationOf(const WriteRecord &record)
    {
        switch (record.kind) {
        case WriteKind::Pulse:
            return PlcOperation::Pulse;
        case WriteKind::Register:
            return PlcOperation::WriteRegister;
        case WriteKind::Hold:
        case WriteKind::Coil:
            break;
        }
        return PlcOperation::WriteCoil;
    }

    quint64 m_nextId = 0;
};

// Synthetic snapshot of a not-running machine. homeBits carries M50 (bit 0);
// statusWord1 carries M1 (manual, bit 1), M3 (running, bit 3), M9 (home
// complete, bit 9) and M14 (latched fault, bit 14); faultCode is D110. Per-block
// quality is Valid so every field is usable evidence.
// PLC-HMI-011 adds two defaulted conditions for the manual-command matrix:
// estop (M0) and manualMode (M1). Every pre-existing call site is unchanged.
DeviceSnapshot resetSnapshot(bool m50, bool m9, bool m14, quint16 faultCode,
                             bool running = false, bool connected = true,
                             bool estop = false, bool manualMode = true)
{
    DeviceSnapshotData d;
    d.connected = connected;
    quint16 sw1 = 0;
    if (estop)
        sw1 |= quint16(1) << 0; // M0
    if (manualMode)
        sw1 |= quint16(1) << 1; // M1 manual: no mode-switch side path
    if (running)
        sw1 |= quint16(1) << 3; // M3
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

ControlCoordinator *syntheticCoordinator(ResetTransportRecorder &rec, qint64 &now)
{
    return new ControlCoordinator(rec.make(), ControlCoordinator::Config(),
                                  [&now]() { return now; });
}

// --- reset / 回原点 transport helpers ------------------------------------------
//
// The reset is a single-operation request (the M103 reset pulse); 回原点 is a
// separate command that writes one sustained M50=1. These helpers deliver the
// controller's correlated transport replies (and give the coordinator
// processing opportunities) without assuming when exactly it acts.

void driveSnapshots(ControlCoordinator &c, const DeviceSnapshot &machine, qint64 &now,
                    int steps, qint64 stepMs)
{
    for (int i = 0; i < steps; ++i) {
        now += stepMs;
        c.onSnapshot(machine);
    }
}

// Delivers the terminal completion of the M103 reset pulse submission.
bool completeResetPulse(ResetTransportRecorder &rec, ControlCoordinator &c,
                        bool result = true, const QString &error = QString())
{
    const ResetTransportRecorder::WriteRecord pulse = rec.firstWriteTo(kM103, true);
    if (pulse.requestId == 0)
        return false;
    return rec.complete(c, pulse.requestId, result, error);
}

// Drives the injected clock in bounded steps until the reset produces a
// terminal result (or the window is exhausted). Returns the steps used.
int advanceUntilResetTerminal(ControlCoordinator &c, ResetObservation &obs,
                              const DeviceSnapshot &machine, qint64 &now,
                              int maxSteps, qint64 stepMs)
{
    int steps = 0;
    while (obs.terminalCount() == 0 && steps < maxSteps) {
        now += stepMs;
        c.onSnapshot(machine);
        ++steps;
    }
    return steps;
}

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

    // --- PLC-HMI-010: fire-and-confirm-by-fixed-delay reset ------------------
    // --- OB-1 ---------------------------------------------------------------
    void resetCompletesAfterFixedDelayAndNotBefore();
    // --- OB-2 ---------------------------------------------------------------
    void resetResultIgnoresHomeInProgressFlag();
    void resetSuccessesWithHomeCompleteClearAndLatchedFaultWithNonZeroCode();
    void resetResultIgnoresFaultCodeRegister();
    // --- OB-3 ---------------------------------------------------------------
    void nonAdminResetRejectedVisiblyWithoutSubmission();
    void resetRejectedVisiblyWhileMachineRunning();
    // --- OB-4 ---------------------------------------------------------------
    void resetSignalSubmissionFailureConvergesToVisibleFailure();
    // --- OB-5 ---------------------------------------------------------------
    void linkLossWhileResetPendingConvergesVisibly();
    // --- OB-6 / OB-7 --------------------------------------------------------
    void resetPendingVisibleBeforeTerminalWithNonEmptyDetail();
    void resetNeverStaysPendingIndefinitely();
    // --- PLC-HMI-010 verify-phase requirement-derived edge cases ------------
    void resetNeverCompletesBeforeTheFixedDelayLadder();
    void resetThenModeSwitchWhilePendingBothConvergeIndependently();
    void modeSwitchThenResetWhilePendingBothConvergeIndependently();
    void resetWhileNeverOnlineConvergesVisiblyWithoutIndefinitePending();
    void linkLossExactlyAtTheResetDelayBoundaryYieldsOneNonSuccessTerminal();
    void rejectedResetPulseSubmissionHasExactlyOneVisibleOutcome();
    void repeatedDuplicateResetClicksAreEachRejectedWithOneTerminalSuccess();
    void latchedFaultWithNonZeroCodeIsThePrimaryResetUseCase();

    // --- 2026-09-21: reset is pulse-only, 回原点 is its own command ---------
    // --- OB-1 ---------------------------------------------------------------
    void resetNeverWritesTheHomeStartCoil();
    void homeStartWriteIsNotPulsedAndNotRepeated();
    // --- OB-2 ---------------------------------------------------------------
    void homeStartDoesNotCompleteBeforeTheM50Readback();
    void homeStartTransportCompletionAloneIsNotSuccess();
    // --- OB-3 ---------------------------------------------------------------
    void rejectedHomeStartWriteConvergesToExactlyOneNonSuccessTerminal();
    void failedHomeStartCompletionConvergesToExactlyOneNonSuccessTerminal();
    void homeStartWriteWithoutAnyCompletionConvergesToAVisibleTerminal();
    // --- OB-4 ---------------------------------------------------------------
    void rejectedResetPulseNeverAttemptsTheHomeStartWrite();
    void duplicateHomeStartWhilePendingIsRejectedAndTheWriteHappensOnce();
    // --- OB-5 ---------------------------------------------------------------
    void resetResultIgnoresTheControllerHomeStartBit();

    // --- PLC-HMI-011 OB-6: manual commands without homing -------------------
    void beltJogAndStopGateAcceptedWithHomeCompleteClear();
    void widthJogsAcceptedWithHomeCompleteClear();
    void widthJogsAcceptedWhileHomeStartBitReadsHigh();
    void manualCommandCommonGatesStillRejectVisiblyWithReasons();
    void homeStartRejectedWithVisibleReasonsWhenGatesUnmet();

    // --- PLC-HMI-011 OB-8: simulated controller handshake -------------------
    void simulatorResetPulseAloneDoesNotStartHoming();
    void simulatorHomeStartWriteStartsHomingAndReadsBack();
    void simulatorHomingCompletionClearsTheStartBitAndSetsHomeComplete();
    void simulatorResetPulseStillClearsHomeCompleteFaultAndWidthAdjustment();
    void simulatorGatewayResetPulseAloneLeavesHomingIdle();
    void simulatorGatewayReportsTheHandshakeThroughSnapshots();
};

// --- PLC-HMI-011 OB-1 ---------------------------------------------------------

void OperatorCommandLifecycleTest::resetNeverWritesTheHomeStartCoil()
{
    // User decision 2026-09-21: 复位 and 回原点 are separate commands. An
    // accepted administrator reset on a not-running machine sends the M103
    // reset pulse only; it never writes the home-start coil (M50), so it never
    // starts homing, however long it is observed.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY2(c->reset().accepted, "the administrator reset was not accepted");
    QVERIFY2(rec.hasWriteTo(kM103, true), "the reset did not send the M103 pulse");
    QVERIFY2(!rec.hasWriteTo(kM50, true),
             "the reset wrote the home-start coil before the pulse completed");
    QVERIFY2(obs.results.isEmpty(), "the reset produced a result before its operations");

    // The pulse completes; the reset converges on its own fixed boundary and
    // still never touches the home-start coil.
    QVERIFY2(completeResetPulse(rec, *c), "the M103 pulse completion was not delivered");
    advanceUntilResetTerminal(*c, obs, machine, now, 40, kResetFixedDelayMs);

    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY(obs.results[0].ok);
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));

    driveSnapshots(*c, machine, now, 20, kResetFixedDelayMs);

    QVERIFY2(!rec.hasWriteTo(kM50, true),
             "the reset wrote the home-start coil although 回原点 is a separate command");
    QVERIFY2(!rec.hasWriteTo(kM50, false), "the reset cleared the home-start coil");
    QVERIFY2(rec.pulses.count(kM50) == 0,
             "the reset wrote the home-start coil as a pulse");
    QCOMPARE(obs.terminalCount(), 1);
}

void OperatorCommandLifecycleTest::homeStartWriteIsNotPulsedAndNotRepeated()
{
    // User decision 2026-09-21: 回原点 writes coil 50 with a plain value write,
    // exactly once. It is not a pulse and no second write follows, even after
    // the command has converged and further time passes.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot idle = resetSnapshot(false, true, false, 0);
    c->onSnapshot(idle);

    QSignalSpy results(c.get(), &ControlCoordinator::commandResult);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY2(c->homeStart().accepted, "the 回原点 command was not accepted");
    QCOMPARE(rec.writeCount(kM50, true), 1);
    QCOMPARE(rec.firstWriteTo(kM50, true).kind, ResetTransportRecorder::WriteKind::Coil);
    QVERIFY2(rec.pulses.count(kM50) == 0,
             "the home-start bit was written as a pulse instead of a plain write");
    QVERIFY2(!rec.hasWriteTo(kM50, false),
             "the home-start bit was cleared by a second coil-50 write");
    QCOMPARE(rejected.count(), 0);

    // The M50 readback (HOME poll block) confirms the command: homing is
    // running once the controller reports the home-start bit.
    const DeviceSnapshot homing = resetSnapshot(true, false, false, 0);
    c->onSnapshot(homing);
    QCOMPARE(results.count(), 1);
    QVERIFY(results[0][1].toBool());
    QCOMPARE(results[0][2].toString(), QStringLiteral("回原点已启动"));
    QVERIFY(!c->homeStartInProgress());

    // A long quiet window: no repeat, no clear, no second terminal.
    driveSnapshots(*c, homing, now, 20, kResetFixedDelayMs);
    QCOMPARE(rec.writeCount(kM50, true), 1);
    QVERIFY2(!rec.hasWriteTo(kM50, false),
             "the home-start bit was cleared by a second coil-50 write");
    QCOMPARE(results.count(), 1);
}

// --- PLC-HMI-011 OB-2 ---------------------------------------------------------

void OperatorCommandLifecycleTest::homeStartDoesNotCompleteBeforeTheM50Readback()
{
    // No optimistic success: an accepted 回原点 command stays pending while the
    // controller still reports the home-start bit clear, and converges to
    // exactly one success (回原点已启动) only once the M50 readback confirms it.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot idle = resetSnapshot(false, true, false, 0);
    c->onSnapshot(idle);

    QSignalSpy results(c.get(), &ControlCoordinator::commandResult);

    QVERIFY(c->homeStart().accepted);
    QCOMPARE(rec.writeCount(kM50, true), 1);
    QVERIFY2(results.isEmpty(), "a terminal result appeared before the M50 readback");
    QVERIFY(c->homeStartInProgress());

    // Snapshots that still report the home-start bit clear cannot confirm it.
    driveSnapshots(*c, idle, now, 5, 100);
    QVERIFY2(results.isEmpty(),
             "the 回原点 command reported a result without an M50 readback");
    QVERIFY(c->homeStartInProgress());

    // The readback confirms: exactly one success with the fixed detail, and no
    // second terminal afterwards.
    const DeviceSnapshot homing = resetSnapshot(true, false, false, 0);
    c->onSnapshot(homing);
    QCOMPARE(results.count(), 1);
    QVERIFY(results[0][1].toBool());
    QCOMPARE(results[0][2].toString(), QStringLiteral("回原点已启动"));
    QVERIFY(!c->homeStartInProgress());

    driveSnapshots(*c, homing, now, 10, kResetFixedDelayMs);
    QCOMPARE(results.count(), 1);
}

void OperatorCommandLifecycleTest::homeStartTransportCompletionAloneIsNotSuccess()
{
    // The write's correlated transport completion proves delivery only: the
    // command may not report success while the controller still reports the
    // home-start bit clear (no optimistic machine state).
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot idle = resetSnapshot(false, true, false, 0);
    c->onSnapshot(idle);

    QSignalSpy results(c.get(), &ControlCoordinator::commandResult);

    QVERIFY(c->homeStart().accepted);
    const ResetTransportRecorder::WriteRecord start = rec.firstWriteTo(kM50, true);
    QVERIFY2(start.requestId != 0, "the home-start write was not submitted");
    QVERIFY2(rec.complete(*c, start.requestId, true),
             "the home-start completion was not delivered");

    driveSnapshots(*c, idle, now, 10, 100);
    QVERIFY2(results.isEmpty(),
             "the delivered transport completion alone reported success");
    QVERIFY(c->homeStartInProgress());

    const DeviceSnapshot homing = resetSnapshot(true, false, false, 0);
    c->onSnapshot(homing);
    QCOMPARE(results.count(), 1);
    QVERIFY(results[0][1].toBool());
    QCOMPARE(results[0][2].toString(), QStringLiteral("回原点已启动"));
}

// --- PLC-HMI-011 OB-3 ---------------------------------------------------------

void OperatorCommandLifecycleTest::rejectedHomeStartWriteConvergesToExactlyOneNonSuccessTerminal()
{
    // A home-start write rejected at submission is exactly one visible
    // non-success outcome with a non-empty reason; it never reports 回原点已启动
    // and never leaves the command pending.
    ResetTransportRecorder rec;
    rec.rejectHomeStart = true;
    rec.homeStartRejectReason = QStringLiteral("home-start write rejected by the transport");
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot idle = resetSnapshot(false, true, false, 0);
    c->onSnapshot(idle);

    QSignalSpy results(c.get(), &ControlCoordinator::commandResult);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult r = c->homeStart();
    QVERIFY2(!r.accepted, "a rejected home-start write was accepted");
    QVERIFY2(!r.reason.isEmpty(), "the rejected home-start write had an empty reason");
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::HomeStart);
    QVERIFY(!rejected[0][1].toString().isEmpty());
    QCOMPARE(rec.writeCount(kM50, true), 0);
    QCOMPARE(results.count(), 0);
    QVERIFY(!c->homeStartInProgress());

    // A later confirmation snapshot cannot turn the rejection into a success.
    const DeviceSnapshot homing = resetSnapshot(true, false, false, 0);
    driveSnapshots(*c, homing, now, 10, kResetFixedDelayMs);
    QCOMPARE(results.count(), 0);
    QCOMPARE(rejected.count(), 1);
}

void OperatorCommandLifecycleTest::failedHomeStartCompletionConvergesToExactlyOneNonSuccessTerminal()
{
    // A home-start write accepted at submission whose correlated completion
    // reports failure converges to exactly one visible non-success terminal
    // with a non-empty detail.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot idle = resetSnapshot(false, true, false, 0);
    c->onSnapshot(idle);

    QSignalSpy results(c.get(), &ControlCoordinator::commandResult);

    QVERIFY(c->homeStart().accepted);
    const ResetTransportRecorder::WriteRecord start = rec.firstWriteTo(kM50, true);
    QVERIFY2(start.requestId != 0, "the home-start write was not submitted");
    QVERIFY2(rec.complete(*c, start.requestId, false,
                          QStringLiteral("modbus exception 0x02")),
             "the failed home-start completion was not delivered");

    QCOMPARE(results.count(), 1);
    QVERIFY2(!results[0][1].toBool(), "a failed home-start completion reported success");
    QVERIFY2(!results[0][2].toString().trimmed().isEmpty(),
             "the failed home-start terminal had an empty detail");
    QVERIFY(!c->homeStartInProgress());

    // More snapshots, including a homing one, cannot produce a second terminal.
    const DeviceSnapshot homing = resetSnapshot(true, false, false, 0);
    driveSnapshots(*c, homing, now, 20, kResetFixedDelayMs);
    QCOMPARE(results.count(), 1);
    QVERIFY(!results[0][1].toBool());
}

void OperatorCommandLifecycleTest::homeStartWriteWithoutAnyCompletionConvergesToAVisibleTerminal()
{
    // If the home-start write is accepted but the controller never reports the
    // home-start bit and no completion arrives, the command must not report
    // success and must not stay pending forever: it converges to a visible
    // terminal outcome inside the confirmation deadline.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot idle = resetSnapshot(false, true, false, 0);
    c->onSnapshot(idle);

    QSignalSpy results(c.get(), &ControlCoordinator::commandResult);

    QVERIFY(c->homeStart().accepted);
    QVERIFY2(rec.hasWriteTo(kM50, true), "the home-start write was never submitted");
    QVERIFY(c->homeStartInProgress());
    // Neither the completion nor the M50 readback is ever delivered.

    driveSnapshots(*c, idle, now, 120, 1'000);

    QCOMPARE(results.count(), 1);
    QVERIFY2(!results[0][1].toBool(),
             "an unconfirmed home-start write reported success");
    QVERIFY2(!results[0][2].toString().trimmed().isEmpty(),
             "the unconfirmed home-start terminal had an empty detail");
    QVERIFY(!c->homeStartInProgress());
}

// --- PLC-HMI-011 OB-4 ---------------------------------------------------------

void OperatorCommandLifecycleTest::rejectedResetPulseNeverAttemptsTheHomeStartWrite()
{
    // Brief OB-4: if the reset pulse submission is rejected, the command
    // converges to exactly one visible non-success terminal; no home-start write
    // is attempted after the failure.
    ResetTransportRecorder rec;
    rec.rejectPulses = true;
    rec.pulseRejectReason = QStringLiteral("M103 脉冲发送失败");
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);

    const ControlCoordinator::CommandResult r = c->reset();

    // Exactly one visible outcome: a rejection with a reason, or one non-success
    // terminal with a detail.
    const bool visibleRejection = !r.accepted && !r.reason.isEmpty();
    const bool visibleFailure = obs.terminalCount() == 1 && obs.successCount() == 0
        && !obs.results[0].detail.trimmed().isEmpty();
    QVERIFY2(visibleRejection || visibleFailure,
             qPrintable(QStringLiteral("a rejected reset pulse was not exactly one visible "
                                       "non-success outcome (accepted=%1 terminals=%2)")
                            .arg(r.accepted ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(describeResetResults(obs.results))));
    QVERIFY2(obs.successCount() == 0, "a rejected reset pulse reported success");

    // No home-start write may be attempted after the failure.
    QVERIFY2(!rec.hasWriteTo(kM50, true),
             "a home-start write was attempted after the reset pulse was rejected");
    QVERIFY2(!rec.hasWriteTo(kM50, false),
             "a home-start clear was attempted after the reset pulse was rejected");

    driveSnapshots(*c, machine, now, 20, kResetFixedDelayMs);
    QCOMPARE(rec.writeCount(kM50, true), 0);
    QCOMPARE(obs.successCount(), 0);
    QVERIFY(!c->resetInProgress());
}

void OperatorCommandLifecycleTest::duplicateHomeStartWhilePendingIsRejectedAndTheWriteHappensOnce()
{
    // A duplicate 回原点 while one is pending is visibly rejected with a
    // non-empty reason; the accepted command still writes coil 50 exactly once
    // and converges to a single success.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot idle = resetSnapshot(false, true, false, 0);
    c->onSnapshot(idle);

    QSignalSpy results(c.get(), &ControlCoordinator::commandResult);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->homeStart().accepted);
    const ControlCoordinator::CommandResult duplicate = c->homeStart();
    QVERIFY2(!duplicate.accepted, "a duplicate 回原点 while pending was accepted");
    QVERIFY2(!duplicate.reason.isEmpty(), "the duplicate 回原点 reason was empty");
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::HomeStart);
    QCOMPARE(rec.writeCount(kM50, true), 1);

    const DeviceSnapshot homing = resetSnapshot(true, false, false, 0);
    c->onSnapshot(homing);
    QCOMPARE(results.count(), 1);
    QVERIFY(results[0][1].toBool());
    QCOMPARE(results[0][2].toString(), QStringLiteral("回原点已启动"));

    driveSnapshots(*c, homing, now, 10, kResetFixedDelayMs);
    QCOMPARE(rec.writeCount(kM50, true), 1);
    QCOMPARE(results.count(), 1);
}

// --- PLC-HMI-011 OB-5 ---------------------------------------------------------

void OperatorCommandLifecycleTest::resetResultIgnoresTheControllerHomeStartBit()
{
    // Brief OB-5: the terminal result is identical whether the controller's
    // home-start bit reads low or high. A set home-in-progress bit must not turn
    // the reset into a failure, and the reset never writes the home-start coil
    // itself (user decision 2026-09-21: 回原点 is its own command).
    const auto runReset = [this](bool controllerHomeStartBit, ResetObservation &obs) {
        ResetTransportRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
        c->setRole(Role::Admin);
        c->onConnectionChanged(true);
        const DeviceSnapshot machine =
            resetSnapshot(controllerHomeStartBit, true, false, 0);
        c->onSnapshot(machine);
        QCOMPARE(machine.m50(), controllerHomeStartBit);

        observeReset(*c, obs, this);
        QVERIFY2(c->reset().accepted, "the reset was not accepted");
        QVERIFY(completeResetPulse(rec, *c));
        advanceUntilResetTerminal(*c, obs, machine, now, 40, kResetFixedDelayMs);
        QVERIFY2(!rec.hasWriteTo(kM50, true),
                 "the reset wrote the home-start coil");
    };

    ResetObservation m50Low;
    runReset(false, m50Low);
    ResetObservation m50High;
    runReset(true, m50High);

    QVERIFY2(m50Low.terminalCount() == 1,
             qPrintable(QStringLiteral("M50 low produced %1 reset terminal(s): %2")
                            .arg(m50Low.terminalCount())
                            .arg(describeResetResults(m50Low.results))));
    QVERIFY2(m50High.terminalCount() == 1,
             qPrintable(QStringLiteral("M50 high produced %1 reset terminal(s): %2")
                            .arg(m50High.terminalCount())
                            .arg(describeResetResults(m50High.results))));
    QVERIFY2(m50Low.successCount() == 1 && m50High.successCount() == 1,
             qPrintable(QStringLiteral("the reset result depended on the controller's "
                                       "home-start bit (M50 low: %1; M50 high: %2)")
                            .arg(describeResetResults(m50Low.results))
                            .arg(describeResetResults(m50High.results))));
    QCOMPARE(m50Low.results[0].detail, QStringLiteral("复位完成"));
    QCOMPARE(m50High.results[0].detail, QStringLiteral("复位完成"));
}

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

// --- PLC-HMI-010 OB-1 ---------------------------------------------------------

void OperatorCommandLifecycleTest::resetCompletesAfterFixedDelayAndNotBefore()
{
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY2(c->reset().accepted,
             "precondition: the administrator reset on a not-running machine was not accepted");
    QVERIFY2(rec.pulses.contains(kM103),
             "the accepted reset did not send the reset signal (M103 pulse)");

    QVERIFY2(completeResetPulse(rec, *c),
             "the M103 pulse completion was not delivered");

    // 1 ms before the fixed delay boundary: no terminal result yet.
    now += kResetFixedDelayMs - 1;
    c->onSnapshot(machine);
    QVERIFY2(obs.results.isEmpty(),
             qPrintable(QStringLiteral("the reset completed before the fixed %1 ms boundary; "
                                       "sequence: %2")
                            .arg(kResetFixedDelayMs)
                            .arg(describeResetResults(obs.results))));

    // At the boundary: exactly one terminal success with the fixed detail.
    now += 1;
    c->onSnapshot(machine);
    QCOMPARE(obs.results.size(), 1);
    QVERIFY2(obs.results[0].ok,
             qPrintable(QStringLiteral("the reset terminal was not a success: %1")
                            .arg(describeResetResults(obs.results))));
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
    QVERIFY(!c->resetInProgress());

    // Exactly one terminal: later snapshots must not emit a second result.
    for (int i = 0; i < 10; ++i) {
        now += kResetFixedDelayMs;
        c->onSnapshot(machine);
    }
    QCOMPARE(obs.results.size(), 1);
}

// --- PLC-HMI-010 OB-2 ---------------------------------------------------------

void OperatorCommandLifecycleTest::resetResultIgnoresHomeInProgressFlag()
{
    // Same accepted reset on a not-running machine twice; the only difference is
    // the controller's M50 home-in-progress flag at completion time. A result
    // that still depends on the machine-completion flag would differ across the
    // two runs (brief OB-2).
    ResetObservation m50Low;
    {
        ResetTransportRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
        c->setRole(Role::Admin);
        c->onConnectionChanged(true);
        const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
        c->onSnapshot(machine);
        observeReset(*c, m50Low, this);

        QVERIFY2(c->reset().accepted, "the M50-low reset was not accepted");
        QVERIFY2(completeResetPulse(rec, *c), "the M50-low pulse completion was not delivered");
        now += kResetFixedDelayMs;
        c->onSnapshot(machine);
    }

    ResetObservation m50High;
    {
        ResetTransportRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
        c->setRole(Role::Admin);
        c->onConnectionChanged(true);
        const DeviceSnapshot machine = resetSnapshot(true, true, false, 0);
        c->onSnapshot(machine);
        QVERIFY(machine.m50());
        observeReset(*c, m50High, this);

        QVERIFY2(c->reset().accepted, "the M50-high reset was not accepted");
        QVERIFY2(completeResetPulse(rec, *c),
                 "the M50-high pulse completion was not delivered");
        now += kResetFixedDelayMs;
        c->onSnapshot(machine);
    }

    QVERIFY2(m50Low.terminalCount() == 1,
             qPrintable(QStringLiteral("M50 low produced %1 reset terminal(s): %2")
                            .arg(m50Low.terminalCount())
                            .arg(describeResetResults(m50Low.results))));
    QVERIFY2(m50High.terminalCount() == 1,
             qPrintable(QStringLiteral("M50 high produced %1 reset terminal(s): %2")
                            .arg(m50High.terminalCount())
                            .arg(describeResetResults(m50High.results))));
    QVERIFY2(m50Low.successCount() == 1 && m50High.successCount() == 1,
             qPrintable(QStringLiteral("the reset result depended on the controller's "
                                       "home-in-progress flag (M50 low: %1; M50 high: %2)")
                            .arg(describeResetResults(m50Low.results))
                            .arg(describeResetResults(m50High.results))));
    QCOMPARE(m50Low.results[0].detail, QStringLiteral("复位完成"));
    QCOMPARE(m50High.results[0].detail, QStringLiteral("复位完成"));
}

void OperatorCommandLifecycleTest::resetSuccessesWithHomeCompleteClearAndLatchedFaultWithNonZeroCode()
{
    // The decisive OB-2 case: the home-complete flag is clear, the latched-fault
    // flag is set, and the fault-code register holds a non-zero value. None of
    // them may turn the accepted reset into a failure.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, false, true, 7);
    c->onSnapshot(machine);
    QVERIFY2(!machine.m9(), "test fixture: the home-complete flag was not clear");
    QVERIFY2(machine.m14(), "test fixture: the latched-fault flag was not set");
    QVERIFY2(machine.faultCode() != 0,
             "test fixture: the fault-code register was not non-zero");
    QVERIFY2(!machine.m3(), "test fixture: the machine was not at rest");

    ResetObservation obs;
    observeReset(*c, obs, this);

    const ControlCoordinator::CommandResult r = c->reset();
    QVERIFY2(r.accepted, qPrintable(QStringLiteral("the reset was rejected: %1").arg(r.reason)));
    QVERIFY2(rec.pulses.contains(kM103), "the accepted reset did not send the reset signal");

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    now += kResetFixedDelayMs;
    c->onSnapshot(machine);

    QVERIFY2(obs.terminalCount() == 1,
             qPrintable(QStringLiteral("a clear home-complete flag plus a latched fault changed "
                                       "the result: %1 terminal(s): %2")
                            .arg(obs.terminalCount())
                            .arg(describeResetResults(obs.results))));
    QVERIFY2(obs.results[0].ok,
             qPrintable(QStringLiteral("a latched fault made the reset fail: %1")
                            .arg(describeResetResults(obs.results))));
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
}

void OperatorCommandLifecycleTest::resetResultIgnoresFaultCodeRegister()
{
    // Fault-code register non-zero with every flag clear must not change the
    // accepted reset result.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 9);
    c->onSnapshot(machine);
    QVERIFY(machine.faultCode() != 0);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY(c->reset().accepted);
    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");
    now += kResetFixedDelayMs;
    c->onSnapshot(machine);

    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY2(obs.results[0].ok,
             qPrintable(QStringLiteral("a non-zero fault code made the reset fail: %1")
                            .arg(describeResetResults(obs.results))));
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
}

// --- PLC-HMI-010 OB-3 ---------------------------------------------------------

void OperatorCommandLifecycleTest::nonAdminResetRejectedVisiblyWithoutSubmission()
{
    for (const Role role : {Role::Anonymous, Role::Operator}) {
        ResetTransportRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
        c->setRole(role);
        c->onConnectionChanged(true);
        c->onSnapshot(resetSnapshot(false, true, false, 0));

        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

        const ControlCoordinator::CommandResult r = c->reset();
        QVERIFY2(!r.accepted, "a non-administrator reset was accepted");
        QVERIFY2(!r.reason.isEmpty(), "the non-administrator reset reason was empty");
        QCOMPARE(rejected.count(), 1);
        QCOMPARE(rejected[0][0].value<Command>(), Command::Reset);
        QVERIFY(!rejected[0][1].toString().isEmpty());
        QVERIFY2(!rec.pulses.contains(kM103),
                 "a non-administrator reset sent the reset signal anyway");
    }
}

void OperatorCommandLifecycleTest::resetRejectedVisiblyWhileMachineRunning()
{
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0, /*running=*/true);
    c->onSnapshot(machine);
    QVERIFY2(machine.m3(), "test fixture: the machine did not report running");

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult r = c->reset();
    QVERIFY2(!r.accepted, "a reset while running was accepted");
    QVERIFY2(!r.reason.isEmpty(), "the while-running reset reason was empty");
    QVERIFY(rejected.count() >= 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::Reset);
    QVERIFY(!rejected[0][1].toString().isEmpty());
    QVERIFY2(!rec.pulses.contains(kM103),
             "a reset while running sent the reset signal anyway");
}

// --- PLC-HMI-010 OB-4 ---------------------------------------------------------

void OperatorCommandLifecycleTest::resetSignalSubmissionFailureConvergesToVisibleFailure()
{
    ResetTransportRecorder rec;
    rec.rejectPulses = true;
    rec.pulseRejectReason = QStringLiteral("reset signal rejected by the transport");
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult r = c->reset();

    // The failure must be visible and must never be reported as success.
    const bool visibleRejection = !r.accepted && !r.reason.isEmpty()
        && rejected.count() == 1 && !rejected[0][1].toString().isEmpty();
    const bool visibleFailure =
        obs.terminalCount() >= 1 && obs.successCount() == 0
        && !obs.results.first().detail.trimmed().isEmpty();
    QVERIFY2(visibleRejection || visibleFailure,
             qPrintable(QStringLiteral("a failed reset-signal submission was silent or claimed "
                                       "success (accepted=%1 reason='%2' terminals=%3)")
                            .arg(r.accepted ? QStringLiteral("true") : QStringLiteral("false"))
                            .arg(r.reason)
                            .arg(describeResetResults(obs.results))));

    // Whatever the path, more time must not produce a success terminal.
    now += kResetFixedDelayMs + 1'000;
    c->onSnapshot(machine);
    QCOMPARE(obs.successCount(), 0);
}

// --- PLC-HMI-010 OB-5 ---------------------------------------------------------

void OperatorCommandLifecycleTest::linkLossWhileResetPendingConvergesVisibly()
{
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY(c->reset().accepted);
    QVERIFY(c->resetInProgress());
    QVERIFY(obs.results.isEmpty());

    // Link loss before the fixed delay elapses.
    c->onConnectionChanged(false);

    QVERIFY2(obs.terminalCount() >= 1,
             "a reset pending at link loss did not converge to a visible terminal");
    QVERIFY2(obs.successCount() == 0,
             qPrintable(QStringLiteral("a reset at link loss reported success: %1")
                            .arg(describeResetResults(obs.results))));
    QVERIFY2(!obs.results.last().detail.trimmed().isEmpty(),
             "the communications-loss reset terminal had an empty detail");
    QVERIFY(!c->resetInProgress());
}

// --- PLC-HMI-010 OB-6 / OB-7 --------------------------------------------------

void OperatorCommandLifecycleTest::resetPendingVisibleBeforeTerminalWithNonEmptyDetail()
{
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    QVector<Command> pendingCommands;
    QObject::connect(c.get(), &ControlCoordinator::commandPending, this,
                     [&pendingCommands](Command cmd) { pendingCommands.append(cmd); });
    QSignalSpy pendingSpy(c.get(), &ControlCoordinator::commandPending);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY(c->reset().accepted);

    // A pending state is visible before any terminal result.
    QVERIFY2(!pendingCommands.isEmpty(),
             "no pending state was emitted before the reset terminal");
    QVERIFY2(pendingCommands.contains(Command::Reset),
             "the emitted pending state did not identify the reset command");
    QVERIFY2(obs.results.isEmpty(),
             "a terminal reset result was emitted before the pending state was observed");

    // The pending state is not terminal and the coordinator reports the command
    // as still in progress until the fixed delay elapses.
    QVERIFY2(c->resetInProgress(),
             "the reset was not in progress after its visible pending state");

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    now += kResetFixedDelayMs;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY(obs.results[0].ok);
}

void OperatorCommandLifecycleTest::resetNeverStaysPendingIndefinitely()
{
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY(c->reset().accepted);
    QVERIFY(c->resetInProgress());

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    // A bounded observation window: the fixed 200 ms delay must converge the
    // command; it may never stay pending indefinitely.
    for (int i = 0; i < 20 && obs.terminalCount() == 0; ++i) {
        now += 50;
        c->onSnapshot(machine);
    }

    QVERIFY2(obs.terminalCount() >= 1,
             "the accepted reset stayed pending without any terminal state");
    QVERIFY2(!c->resetInProgress(), "resetInProgress() stayed true after a terminal result");
    QCOMPARE(obs.successCount(), 1);
    QCOMPARE(obs.results.first().detail, QStringLiteral("复位完成"));

    // Exactly one terminal: no second result after further snapshots.
    for (int i = 0; i < 10; ++i) {
        now += 50;
        c->onSnapshot(machine);
    }
    QCOMPARE(obs.terminalCount(), 1);
}

// --- PLC-HMI-010 verify-phase requirement-derived edge cases -------------------
//
// Added in the VERIFY phase from the behavior-only brief (OB-1, OB-2, OB-3,
// OB-5, OB-7) and the approved contract revision for PLC-HMI-010. No existing
// case or assertion was modified, weakened, skipped, or removed.

void OperatorCommandLifecycleTest::resetNeverCompletesBeforeTheFixedDelayLadder()
{
    // OB-1 boundary: the fixed-delay boundary must be pinned tightly. A ladder
    // of snapshots strictly before the fixed delay must never produce a
    // terminal; the first snapshot at the boundary must produce exactly one.
    // This case is meaningful only if the delay is non-zero: with a 0 ms delay
    // the pre-boundary ladder would already complete the reset.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY2(c->reset().accepted, "the administrator reset was not accepted");
    QVERIFY(rec.pulses.contains(kM103));
    QVERIFY(c->resetInProgress());

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    // Strictly before the boundary: never a terminal, still pending.
    const qint64 ladder[] = {1, 25, 50, 100, 150, 190, kResetFixedDelayMs - 1};
    for (const qint64 step : ladder) {
        now = step;
        c->onSnapshot(machine);
        QVERIFY2(obs.terminalCount() == 0,
                 qPrintable(QStringLiteral("a terminal reset result appeared at %1 ms, "
                                           "before the fixed %2 ms boundary: %3")
                                .arg(now)
                                .arg(kResetFixedDelayMs)
                                .arg(describeResetResults(obs.results))));
        QVERIFY2(c->resetInProgress(),
                 qPrintable(QStringLiteral("resetInProgress() was already false at %1 ms, "
                                           "before the fixed %2 ms boundary")
                                .arg(now)
                                .arg(kResetFixedDelayMs)));
    }

    // At the boundary: exactly one success terminal with the fixed detail.
    now = kResetFixedDelayMs;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY(obs.results[0].ok);
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
    QVERIFY(!c->resetInProgress());

    // No second terminal afterwards.
    for (const qint64 step : {qint64(201), qint64(400), qint64(1000)}) {
        now = step;
        c->onSnapshot(machine);
        QCOMPARE(obs.terminalCount(), 1);
    }
}

void OperatorCommandLifecycleTest::resetThenModeSwitchWhilePendingBothConvergeIndependently()
{
    // Brief boundary: a reset issued while a mode switch may follow, and vice
    // versa, must not be coupled through the mode coil. The second command must
    // neither be silently swallowed nor spuriously rejected while the reset is
    // pending, and the reset must still converge to its own single success.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);
    QVERIFY(machine.m1());  // manual mode: a switch to auto is legitimate
    QVERIFY(machine.m9());  // homed
    QVERIFY(!machine.m3()); // not running

    ResetObservation obs;
    observeReset(*c, obs, this);
    QSignalSpy accepted(c.get(), &ControlCoordinator::commandAccepted);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY2(c->reset().accepted, "the administrator reset was not accepted");
    QVERIFY(rec.pulses.contains(kM103));
    QVERIFY(c->resetInProgress());

    // The mode switch is a legitimate, different command: it must be accepted
    // (no spurious rejection caused by the pending reset).
    const ControlCoordinator::CommandResult mode = c->setMode(true);
    QVERIFY2(mode.accepted,
             qPrintable(QStringLiteral("a legitimate mode switch was rejected while a "
                                       "reset was pending: %1")
                            .arg(mode.reason)));
    QVERIFY2(accepted.count() >= 1, "the accepted mode switch was not visible");
    QVERIFY2(rejected.count() == 0,
             "the mode switch was visibly rejected although it is legitimate");

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    // The reset still converges independently at its own boundary.
    now = kResetFixedDelayMs;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY2(obs.results[0].ok,
             qPrintable(QStringLiteral("the reset did not succeed independently of the "
                                       "mode switch: %1")
                            .arg(describeResetResults(obs.results))));
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
    QVERIFY(!c->resetInProgress());

    // Further snapshots never produce a second reset terminal.
    now += kResetFixedDelayMs;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
}

void OperatorCommandLifecycleTest::modeSwitchThenResetWhilePendingBothConvergeIndependently()
{
    // Reverse overlap: a mode switch is already pending (its write was recorded
    // but the snapshot never confirms it) when a reset arrives. The reset is
    // legitimate (admin, homed, not running) and must be accepted and converge
    // to exactly one success at the fixed delay, not be rejected because of the
    // pending mode switch and not stay pending forever.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    QSignalSpy accepted(c.get(), &ControlCoordinator::commandAccepted);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult mode = c->setMode(true);
    QVERIFY2(mode.accepted, qPrintable(QStringLiteral("the mode switch was not accepted: %1")
                                           .arg(mode.reason)));
    QVERIFY2(accepted.count() >= 1, "the pending mode switch was not visibly accepted");

    ResetObservation obs;
    observeReset(*c, obs, this);

    // The reset must not be spuriously rejected because a mode switch is pending.
    const ControlCoordinator::CommandResult r = c->reset();
    QVERIFY2(r.accepted,
             qPrintable(QStringLiteral("the reset was rejected while a mode switch was "
                                       "pending: %1")
                            .arg(r.reason)));
    QVERIFY2(rec.pulses.contains(kM103), "the accepted reset did not send the M103 pulse");

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    now = kResetFixedDelayMs;
    c->onSnapshot(machine);

    QVERIFY2(obs.terminalCount() == 1,
             qPrintable(QStringLiteral("the reset produced %1 terminal(s) while a mode "
                                       "switch was pending: %2")
                            .arg(obs.terminalCount())
                            .arg(describeResetResults(obs.results))));
    QVERIFY2(obs.results[0].ok,
             qPrintable(QStringLiteral("the reset did not succeed while a mode switch was "
                                       "pending: %1")
                            .arg(describeResetResults(obs.results))));
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
    QVERIFY2(!c->resetInProgress(),
             "resetInProgress() stayed true after the reset terminal");
    QVERIFY2(rejected.count() == 0,
             "the legitimate reset was visibly rejected while a mode switch was pending");

    now += kResetFixedDelayMs;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
}

void OperatorCommandLifecycleTest::resetWhileNeverOnlineConvergesVisiblyWithoutIndefinitePending()
{
    // OB-5/OB-7: a reset requested while the coordinator has never been online
    // (or is offline) must not stay pending indefinitely and must never report
    // success without a sent signal. Either it is rejected visibly with a reason
    // and no signal is sent, or it converges to a non-success terminal.
    {
        // Never online: connection reported down before any snapshot.
        ResetTransportRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
        c->setRole(Role::Admin);
        c->onConnectionChanged(false);
        c->onSnapshot(resetSnapshot(false, true, false, 0, /*running=*/false,
                                    /*connected=*/false));

        ResetObservation obs;
        observeReset(*c, obs, this);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

        const ControlCoordinator::CommandResult r = c->reset();
        now += kResetFixedDelayMs + 1'000;
        c->onSnapshot(resetSnapshot(false, true, false, 0, false, false));

        const bool visibleRejection =
            !r.accepted && !r.reason.isEmpty() && rejected.count() == 1
            && !rejected[0][1].toString().isEmpty() && !rec.pulses.contains(kM103);
        const bool visibleNonSuccessTerminal =
            obs.terminalCount() == 1 && obs.successCount() == 0
            && !obs.results[0].detail.trimmed().isEmpty();
        QVERIFY2(visibleRejection || visibleNonSuccessTerminal,
                 qPrintable(QStringLiteral("an offline reset was silent, stayed pending, or "
                                           "claimed success (accepted=%1 terminals=%2)")
                                .arg(r.accepted ? QStringLiteral("true")
                                                : QStringLiteral("false"))
                                .arg(describeResetResults(obs.results))));
        QVERIFY2(obs.successCount() == 0, "an offline reset reported success");
        QVERIFY2(!c->resetInProgress(),
                 "an offline reset left resetInProgress() true indefinitely");
    }

    {
        // Accepted online, then the link is lost without any further snapshot:
        // the pending reset must converge at the loss event, not stay pending.
        ResetTransportRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
        c->setRole(Role::Admin);
        c->onConnectionChanged(true);
        const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
        c->onSnapshot(machine);

        ResetObservation obs;
        observeReset(*c, obs, this);

        QVERIFY(c->reset().accepted);
        QVERIFY(c->resetInProgress());

        c->onConnectionChanged(false); // no snapshot follows

        QCOMPARE(obs.terminalCount(), 1);
        QVERIFY2(obs.successCount() == 0,
                 qPrintable(QStringLiteral("a reset converged to success at link loss: %1")
                                .arg(describeResetResults(obs.results))));
        QVERIFY2(!obs.results[0].detail.trimmed().isEmpty(),
                 "the link-loss reset terminal had an empty detail");
        QVERIFY2(!c->resetInProgress(),
                 "resetInProgress() stayed true after the link-loss terminal");
    }
}

void OperatorCommandLifecycleTest::linkLossExactlyAtTheResetDelayBoundaryYieldsOneNonSuccessTerminal()
{
    // OB-5 boundary: the link is lost at exactly the fixed-delay instant, with
    // no snapshot delivered afterwards. Exactly one terminal must be produced,
    // it must not be a success, and it must not be reported as both success and
    // communications loss.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);

    QVERIFY(c->reset().accepted);
    QVERIFY(rec.pulses.contains(kM103));

    // Reach exactly the boundary, then drop the link before feeding a snapshot.
    now = kResetFixedDelayMs;
    c->onConnectionChanged(false);

    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY2(obs.successCount() == 0,
             qPrintable(QStringLiteral("the boundary link loss produced a success: %1")
                            .arg(describeResetResults(obs.results))));
    QVERIFY2(!obs.results[0].detail.trimmed().isEmpty(),
             "the boundary link-loss terminal had an empty detail");
    QVERIFY2(!c->resetInProgress(),
             "resetInProgress() stayed true after the boundary link-loss terminal");

    // No late success may arrive after the loss, at or beyond the boundary.
    now = kResetFixedDelayMs + 1;
    c->onSnapshot(machine);
    now = kResetFixedDelayMs + 1'000;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
    QCOMPARE(obs.successCount(), 0);
}

void OperatorCommandLifecycleTest::rejectedResetPulseSubmissionHasExactlyOneVisibleOutcome()
{
    // OB-4: when the M103 pulse submission is rejected (not merely lost), the
    // command must produce exactly one visible outcome: either a visible
    // rejection with a non-empty reason and no terminal, or a single non-success
    // terminal with a non-empty detail. Never success, never both, never pending.
    ResetTransportRecorder rec;
    rec.rejectPulses = true;
    rec.pulseRejectReason = QStringLiteral("M103 脉冲发送失败");
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult r = c->reset();

    const bool visibleRejection =
        !r.accepted && !r.reason.isEmpty() && rejected.count() == 1
        && !rejected[0][1].toString().isEmpty() && obs.terminalCount() == 0;
    const bool visibleNonSuccessTerminal =
        obs.terminalCount() == 1 && obs.successCount() == 0
        && !obs.results[0].detail.trimmed().isEmpty() && rejected.count() == 0;
    QVERIFY2(visibleRejection || visibleNonSuccessTerminal,
             qPrintable(QStringLiteral("a rejected M103 submission was not exactly one "
                                       "visible outcome (accepted=%1 reason='%2' "
                                       "terminals=%3 rejections=%4)")
                            .arg(r.accepted ? QStringLiteral("true")
                                            : QStringLiteral("false"))
                            .arg(r.reason)
                            .arg(describeResetResults(obs.results))
                            .arg(rejected.count())));
    QVERIFY2(obs.successCount() == 0, "a rejected M103 submission reported success");
    QVERIFY2(!c->resetInProgress(),
             "a rejected M103 submission left resetInProgress() true");

    // More time and snapshots must not turn the failure into a success.
    now += kResetFixedDelayMs + 1'000;
    c->onSnapshot(machine);
    QCOMPARE(obs.successCount(), 0);
    QVERIFY(!c->resetInProgress());
}

void OperatorCommandLifecycleTest::repeatedDuplicateResetClicksAreEachRejectedWithOneTerminalSuccess()
{
    // OB-3/OB-7: repeated duplicate clicks while the reset is pending must each
    // be rejected visibly with a non-empty reason, and the accepted reset must
    // still produce exactly one success terminal with the fixed detail.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, false, 0);
    c->onSnapshot(machine);

    ResetObservation obs;
    observeReset(*c, obs, this);
    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    QVERIFY(c->reset().accepted);
    QVERIFY(c->resetInProgress());

    for (int click = 0; click < 3; ++click) {
        const ControlCoordinator::CommandResult duplicate = c->reset();
        QVERIFY2(!duplicate.accepted,
                 qPrintable(QStringLiteral("duplicate reset click %1 was accepted")
                                .arg(click)));
        QVERIFY2(!duplicate.reason.isEmpty(),
                 qPrintable(QStringLiteral("duplicate reset click %1 had an empty reason")
                                .arg(click)));
    }

    QCOMPARE(rejected.count(), 3);
    for (int i = 0; i < rejected.count(); ++i) {
        QCOMPARE(rejected[i][0].value<Command>(), Command::Reset);
        QVERIFY(!rejected[i][1].toString().isEmpty());
    }
    QCOMPARE(obs.terminalCount(), 0); // still pending: no premature terminal

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    now = kResetFixedDelayMs;
    c->onSnapshot(machine);

    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY(obs.results[0].ok);
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
    QVERIFY(!c->resetInProgress());

    now += kResetFixedDelayMs;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
    QCOMPARE(obs.successCount(), 1);
}

void OperatorCommandLifecycleTest::latchedFaultWithNonZeroCodeIsThePrimaryResetUseCase()
{
    // OB-2 primary real-world use case: the machine is homed but carries a
    // latched fault and a non-zero fault code. The reset must be accepted, must
    // send M103 only (never M104), and must converge to exactly one success with
    // the fixed detail. No fault-related wording may appear in the result.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, true, true, 42);
    c->onSnapshot(machine);
    QVERIFY2(machine.m9(), "test fixture: the machine was not homed");
    QVERIFY2(machine.m14(), "test fixture: the latched fault was not set");
    QVERIFY2(machine.faultCode() == 42, "test fixture: the fault code was not 42");
    QVERIFY2(!machine.m3(), "test fixture: the machine was not at rest");

    ResetObservation obs;
    observeReset(*c, obs, this);

    const ControlCoordinator::CommandResult r = c->reset();
    QVERIFY2(r.accepted, qPrintable(QStringLiteral("the latched-fault reset was rejected: %1")
                                        .arg(r.reason)));
    QVERIFY2(rec.pulses.contains(kM103), "the reset did not send the M103 pulse");

    // The reset is fire-and-confirm: it never writes the mode coil M104.
    for (const QPair<quint16, bool> &write : rec.coils) {
        QVERIFY2(write.first != kM104,
                 "the reset wrote the mode coil M104");
    }

    QVERIFY2(completeResetPulse(rec, *c), "the pulse completion was not delivered");

    now = kResetFixedDelayMs;
    c->onSnapshot(machine);

    QCOMPARE(obs.terminalCount(), 1);
    QVERIFY2(obs.results[0].ok,
             qPrintable(QStringLiteral("the latched-fault reset did not succeed: %1")
                            .arg(describeResetResults(obs.results))));
    QCOMPARE(obs.results[0].detail, QStringLiteral("复位完成"));
    QVERIFY2(!obs.results[0].detail.contains(QStringLiteral("故障")),
             "the reset result claimed a fault state");
    QVERIFY(!c->resetInProgress());

    now += kResetFixedDelayMs;
    c->onSnapshot(machine);
    QCOMPARE(obs.terminalCount(), 1);
}

// --- PLC-HMI-011 OB-6 ---------------------------------------------------------

void OperatorCommandLifecycleTest::beltJogAndStopGateAcceptedWithHomeCompleteClear()
{
    // Brief OB-6: with the controller online, manual mode, not running, no
    // emergency stop, no latched fault and home-complete CLEAR, the belt-jog
    // command (coil 108, hold) and the stop-gate command (coil 109, latched)
    // are accepted.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, /*m9=*/false, false, 0);
    c->onSnapshot(machine);
    QVERIFY2(!machine.m9(), "test fixture: home-complete must be clear");

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult jog = c->manualHold(kM108, true);
    QVERIFY2(jog.accepted,
             qPrintable(QStringLiteral("the belt-jog command was rejected while "
                                       "home-complete is clear: %1")
                            .arg(jog.reason)));
    QVERIFY2(rec.hasWriteTo(kM108, true),
             "the accepted belt-jog command did not write coil 108");
    c->manualHold(kM108, false); // release

    const ControlCoordinator::CommandResult gate = c->manualLatch(kM109, true);
    QVERIFY2(gate.accepted,
             qPrintable(QStringLiteral("the stop-gate command was rejected while "
                                       "home-complete is clear: %1")
                            .arg(gate.reason)));
    QVERIFY2(rec.hasWriteTo(kM109, true),
             "the accepted stop-gate command did not write coil 109");

    QCOMPARE(rejected.count(), 0);
}

void OperatorCommandLifecycleTest::widthJogsAcceptedWithHomeCompleteClear()
{
    // User decision 2026-09-21: no manual command requires homing completion.
    // Under the same conditions as the belt-jog case, the two width-jog
    // commands (coils 106 and 107, hold) are accepted while home-complete is
    // clear, and they write their coils.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(false, /*m9=*/false, false, 0);
    c->onSnapshot(machine);
    QVERIFY2(!machine.m9(), "test fixture: home-complete must be clear");

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult fwd = c->manualHold(kM106, true);
    QVERIFY2(fwd.accepted,
             qPrintable(QStringLiteral("the width-forward command was rejected while "
                                       "home-complete is clear: %1")
                            .arg(fwd.reason)));
    QVERIFY2(rec.hasWriteTo(kM106, true),
             "the accepted width-forward command did not write coil 106");
    c->manualHold(kM106, false); // release

    const ControlCoordinator::CommandResult rev = c->manualHold(kM107, true);
    QVERIFY2(rev.accepted,
             qPrintable(QStringLiteral("the width-reverse command was rejected while "
                                       "home-complete is clear: %1")
                            .arg(rev.reason)));
    QVERIFY2(rec.hasWriteTo(kM107, true),
             "the accepted width-reverse command did not write coil 107");

    QCOMPARE(rejected.count(), 0);
}

void OperatorCommandLifecycleTest::widthJogsAcceptedWhileHomeStartBitReadsHigh()
{
    // User decision 2026-09-21: the width jogs are accepted while the
    // home-start bit (coil 50) reads high, exactly like the belt jog and the
    // stop gate. The PLC's own M61/M50 ladder conditions are unchanged; the HMI
    // no longer pre-empts them.
    ResetTransportRecorder rec;
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
    c->setRole(Role::Admin);
    c->onConnectionChanged(true);
    const DeviceSnapshot machine = resetSnapshot(/*m50=*/true, /*m9=*/true, false, 0);
    c->onSnapshot(machine);
    QVERIFY2(machine.m50(), "test fixture: the home-start bit must read high");
    QVERIFY2(machine.m9(), "test fixture: home-complete must be set");

    QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult fwd = c->manualHold(kM106, true);
    QVERIFY2(fwd.accepted,
             qPrintable(QStringLiteral("the width-forward command was rejected while "
                                       "the home-start bit reads high: %1")
                            .arg(fwd.reason)));
    QVERIFY2(rec.hasWriteTo(kM106, true),
             "the accepted width-forward command did not write coil 106");
    c->manualHold(kM106, false); // release

    const ControlCoordinator::CommandResult rev = c->manualHold(kM107, true);
    QVERIFY2(rev.accepted,
             qPrintable(QStringLiteral("the width-reverse command was rejected while "
                                       "the home-start bit reads high: %1")
                            .arg(rev.reason)));
    QVERIFY2(rec.hasWriteTo(kM107, true),
             "the accepted width-reverse command did not write coil 107");

    QCOMPARE(rejected.count(), 0);
}

void OperatorCommandLifecycleTest::manualCommandCommonGatesStillRejectVisiblyWithReasons()
{
    // Brief OB-6: every one of the four manual commands still requires
    // administrator role, manual mode, machine not running, no emergency stop,
    // no latched fault and an online fresh snapshot. Each unmet condition must
    // remain visible in the operator-facing reason.
    struct Scenario
    {
        const char *name;
        Role role;
        bool online;
        bool haveSnapshot;
        bool manualMode;
        bool running;
        bool estop;
        bool latchedFault;
    };
    const Scenario scenarios[] = {
        {"anonymous role", Role::Anonymous, true, true, true, false, false, false},
        {"operator role", Role::Operator, true, true, true, false, false, false},
        {"offline", Role::Admin, false, true, true, false, false, false},
        {"no fresh snapshot", Role::Admin, true, false, true, false, false, false},
        {"automatic mode", Role::Admin, true, true, false, false, false, false},
        {"machine running", Role::Admin, true, true, true, true, false, false},
        {"emergency stop", Role::Admin, true, true, true, false, true, false},
        {"latched fault", Role::Admin, true, true, true, false, false, true},
    };

    const quint16 addresses[] = {kM106, kM107, kM108, kM109};

    for (const Scenario &s : scenarios) {
        for (const quint16 address : addresses) {
            ResetTransportRecorder rec;
            qint64 now = 0;
            std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
            c->setRole(s.role);
            c->onConnectionChanged(s.online);
            if (s.haveSnapshot) {
                // M9 is set so that only the scenario's own condition blocks the
                // command; the width jogs are additionally blocked by M9 alone
                // only in the home-complete-clear cases above.
                c->onSnapshot(resetSnapshot(false, /*m9=*/true, s.latchedFault, 0,
                                            s.running, s.online, s.estop, s.manualMode));
            }

            QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

            const ControlCoordinator::CommandResult r = (address == kM109)
                ? c->manualLatch(address, true)
                : c->manualHold(address, true);

            QVERIFY2(!r.accepted,
                     qPrintable(QStringLiteral("%1: manual command %2 was accepted")
                                    .arg(QString::fromLatin1(s.name))
                                    .arg(address)));
            QVERIFY2(!r.reason.isEmpty(),
                     qPrintable(QStringLiteral("%1: manual command %2 rejection reason was "
                                               "empty")
                                    .arg(QString::fromLatin1(s.name))
                                    .arg(address)));
            QVERIFY2(rejected.count() >= 1,
                     qPrintable(QStringLiteral("%1: manual command %2 rejection was not "
                                               "visible on the signal surface")
                                    .arg(QString::fromLatin1(s.name))
                                    .arg(address)));
            QVERIFY2(!rec.hasWriteTo(address, true),
                     qPrintable(QStringLiteral("%1: rejected manual command %2 wrote its coil")
                                    .arg(QString::fromLatin1(s.name))
                                    .arg(address)));
        }
    }
}

void OperatorCommandLifecycleTest::homeStartRejectedWithVisibleReasonsWhenGatesUnmet()
{
    // 回原点 keeps its own gates: online, manual mode, machine at rest, no
    // emergency stop, no latched fault and no homing already in progress. Each
    // unmet condition is visibly rejected with a non-empty reason, and no
    // home-start write is attempted.
    struct Scenario
    {
        const char *name;
        bool online;
        bool manualMode;
        bool running;
        bool estop;
        bool latchedFault;
        bool homing;
    };
    const Scenario scenarios[] = {
        {"offline", false, true, false, false, false, false},
        {"automatic mode", true, false, false, false, false, false},
        {"machine running", true, true, true, false, false, false},
        {"emergency stop", true, true, false, true, false, false},
        {"latched fault", true, true, false, false, true, false},
        {"already homing", true, true, false, false, false, true},
    };

    for (const Scenario &sc : scenarios) {
        ResetTransportRecorder rec;
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(syntheticCoordinator(rec, now));
        c->setRole(Role::Admin);
        c->onConnectionChanged(sc.online);
        c->onSnapshot(resetSnapshot(sc.homing, true, sc.latchedFault, 0, sc.running,
                                    sc.online, sc.estop, sc.manualMode));

        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

        const ControlCoordinator::CommandResult r = c->homeStart();
        QVERIFY2(!r.accepted,
                 qPrintable(QStringLiteral("%1: 回原点 was accepted")
                                .arg(QString::fromLatin1(sc.name))));
        QVERIFY2(!r.reason.isEmpty(),
                 qPrintable(QStringLiteral("%1: the 回原点 rejection reason was empty")
                                .arg(QString::fromLatin1(sc.name))));
        QVERIFY2(rejected.count() >= 1,
                 qPrintable(QStringLiteral("%1: the 回原点 rejection was not visible on the "
                                           "signal surface")
                                .arg(QString::fromLatin1(sc.name))));
        QVERIFY2(!rec.hasWriteTo(kM50, true),
                 qPrintable(QStringLiteral("%1: the rejected 回原点 wrote the home-start coil")
                                .arg(QString::fromLatin1(sc.name))));
    }
}

// --- PLC-HMI-011 OB-8 ---------------------------------------------------------

void OperatorCommandLifecycleTest::simulatorResetPulseAloneDoesNotStartHoming()
{
    // Brief OB-8: the simulator must NOT treat the reset pulse alone as starting
    // homing. After the pulse alone it reports homing not in progress and
    // home-complete clear.
    SimulationClock clock;
    H3uSimulationModel m(clock);

    m.writeCoil(kM103, true);
    m.writeCoil(kM103, false);

    QVERIFY2(!m.readCoil(kM50),
             "the reset pulse alone started homing (M50 reads back 1)");
    QVERIFY2(!m.readCoil(kM61),
             "the reset pulse alone reported home-complete (M61 set)");

    m.advance(5);
    QVERIFY2(!m.readCoil(kM50), "homing started without a home-start write");
    QVERIFY2(!m.readCoil(kM61), "home-complete appeared without a home-start write");
}

void OperatorCommandLifecycleTest::simulatorHomeStartWriteStartsHomingAndReadsBack()
{
    // Brief OB-8: after one home-start write of 1 the simulated controller
    // reports homing in progress; while homing is in progress the home-start bit
    // reads back 1.
    SimulationClock clock;
    H3uSimulationModel m(clock);

    m.writeCoil(kM50, true);

    QVERIFY2(m.readCoil(kM50),
             "the home-start bit did not read back 1 while homing is in progress");
    QVERIFY2(!m.readCoil(kM61), "home-complete was reported before homing finished");

    m.advance(1);
    QVERIFY2(m.readCoil(kM50),
             "the home-start bit did not stay asserted while homing is in progress");
    QVERIFY2(!m.readCoil(kM61), "home-complete was reported before homing finished");
}

void OperatorCommandLifecycleTest::simulatorHomingCompletionClearsTheStartBitAndSetsHomeComplete()
{
    // Brief OB-8: when the simulated homing finishes, the simulated controller
    // clears the home-start bit by itself and sets home-complete.
    SimulationClock clock;
    H3uSimulationModel m(clock);

    m.writeCoil(kM50, true);
    m.advance(2); // the established home-return duration

    QVERIFY2(!m.readCoil(kM50),
             "the simulated controller did not clear the home-start bit itself");
    QVERIFY2(m.readCoil(kM61),
             "the simulated controller did not set home-complete after homing");
}

void OperatorCommandLifecycleTest::simulatorResetPulseStillClearsHomeCompleteFaultAndWidthAdjustment()
{
    // Brief OB-8: the reset pulse still clears the home-complete bit, the
    // latched-fault bit, the fault-code register and any in-progress width
    // adjustment.
    SimulationClock clock;
    H3uSimulationModel m(clock);

    // Reach home-complete through the handshake (reset pulse + home-start).
    m.writeCoil(kM103, true);
    m.writeCoil(kM103, false);
    m.writeCoil(kM50, true);
    for (int i = 0; i < 10 && !m.readCoil(kM61); ++i)
        m.advance(1);
    QVERIFY2(m.readCoil(kM61), "precondition: home-complete must be reachable");

    // A width adjustment is in progress and a fault is latched (software estop
    // sets M14 and D110=1; the request is then released so the fault stays
    // latched until a reset).
    m.writeRegister(220, 15);
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QVERIFY2(m.readCoil(34), "precondition: the width adjustment must be in progress");
    m.writeCoil(100, true);
    QVERIFY2(m.readCoil(14), "precondition: the fault must be latched");
    QCOMPARE(m.readRegister(110), quint16(1));
    m.writeCoil(100, false);
    QVERIFY2(!m.readCoil(0), "precondition: the estop request must be released");

    // The reset pulse clears all of it and does not itself start homing.
    m.writeCoil(kM103, true);
    m.writeCoil(kM103, false);

    QVERIFY2(!m.readCoil(kM61), "the reset pulse must clear the home-complete bit");
    QVERIFY2(!m.readCoil(kM14), "the reset pulse must clear the latched fault");
    QCOMPARE(m.readRegister(110), quint16(0));
    QVERIFY2(!m.readCoil(34),
             "the reset pulse must clear an in-progress width adjustment");
    QVERIFY2(!m.readCoil(44), "the reset pulse must clear a width-adjust success");
    QVERIFY2(!m.readCoil(45), "the reset pulse must clear a width-adjust failure");
    QVERIFY2(!m.readCoil(kM50), "the reset pulse alone must not start homing");
}

void OperatorCommandLifecycleTest::simulatorGatewayResetPulseAloneLeavesHomingIdle()
{
    // Brief OB-8 through the composed simulated gateway: the reset pulse alone
    // does not start homing, and it still clears home-complete.
    SimulatedPlcGateway gw;
    gw.start();
    QVERIFY(gw.isOnline());

    // Reach home-complete through the handshake.
    QVERIFY(gw.submitPulse(kM103).accepted);
    gw.tick();
    QVERIFY(gw.submitWriteCoil(kM50, true).accepted);
    for (int i = 0; i < 10 && !gw.lastSnapshot().m9(); ++i)
        gw.tick();
    QVERIFY2(gw.lastSnapshot().m9(),
             "precondition: home-complete must be reachable through the handshake");

    // The reset pulse alone: home-complete clears, homing stays idle.
    QVERIFY(gw.submitPulse(kM103).accepted);
    gw.tick();

    QVERIFY2(!gw.lastSnapshot().m9(), "the reset pulse must clear home-complete");
    QVERIFY2(!gw.lastSnapshot().m50(), "the reset pulse alone started homing");
}

void OperatorCommandLifecycleTest::simulatorGatewayReportsTheHandshakeThroughSnapshots()
{
    // Brief OB-8: one home-start write of 1 makes the simulated controller
    // report homing in progress with the home-start bit reading back 1, and
    // completion clears the bit and reports home-complete.
    SimulatedPlcGateway gw;
    gw.start();
    QVERIFY(gw.isOnline());
    QVERIFY2(!gw.lastSnapshot().m50(), "precondition: homing must start idle");
    QVERIFY2(!gw.lastSnapshot().m9(), "precondition: home-complete must start clear");

    const SubmissionResult start = gw.submitWriteCoil(kM50, true);
    QVERIFY2(start.accepted, qPrintable(start.immediate_rejection_reason));

    gw.tick();
    QVERIFY2(gw.lastSnapshot().m50(), "the home-start write did not start homing");
    QVERIFY2(!gw.lastSnapshot().m9(), "home-complete appeared before homing finished");

    gw.tick();
    QVERIFY2(!gw.lastSnapshot().m50(),
             "the simulated controller did not clear the home-start bit itself");
    QVERIFY2(gw.lastSnapshot().m9(),
             "the simulated controller did not report home-complete");
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
