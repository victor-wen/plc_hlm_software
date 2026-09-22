#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

#include "application/interlock_rules.h"
#include "application/permission_policy.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"

namespace hlm {

// Unified business coordination of 复位、配方调宽、模式、启动、停止、软件急停、
// 手动命令和屏蔽 (spec §10, §11.4, §13). Owned by the UI main thread (§7.1);
// talks to the PLC only through the IPlcGateway port (async, thread-safe).
//
// Design:
//  - Every command goes through permission + interlock checks; results are
//    reported via signals, never optimistic (spec §11.2).
//  - A duplicate/in-progress request for any command is rejected with a
//    visible commandRejected(cmd, reason); no entry point returns silently
//    (.ai/project-contract.yaml invariants: visible rejection, no silent
//    duplicate).
//  - Pulses (M101/M102/M103/M43) are submitted through a small transport
//    abstraction (PulseTransport) so the coordinator stays testable against a
//    fake or the SimulatedPlcGateway; the real worker thread wires the
//    PulseStateMachine callbacks into the gateway (spec §7.2, §8.5).
//  - Command lifecycle is tracked per flow: reset is a
//    fire-and-confirm-by-fixed-delay request that sends the M103 pulse only and
//    converges after a fixed 200 ms (spec §10.2, PLC-HMI-010); adjust decides
//    from M34 + D130 against the saved target (user decision 2026-09-22),
//    start waits for M3, stop waits for M3=0.
//    Timeouts converge to the actual PLC state (spec §13).
//  - Hold/latch/bypass commands are accepted+pending on submission and only
//    report success when a confirmed snapshot shows the requested machine
//    state; a transport write rejection is visible and a defensive timeout
//    converges an unconfirmed command to failure.
//  - M100 is never auto-cleared (spec §10.6, §11.5, §13).
class ControlCoordinator : public QObject
{
    Q_OBJECT

public:
    // Pulse transport abstraction (spec §8.5, PLC-HMI-003 D5). The real
    // worker thread routes these into the gateway's submission port; tests use
    // a recording fake. Every callback returns the structured SubmissionResult
    // (accepted + request identity + generation, or a visible rejection).
    struct PulseTransport {
        std::function<SubmissionResult(quint16 address)> startPulse;
        // Continuous (hold) command: write 1 on press, 0 on release.
        std::function<SubmissionResult(quint16 address, bool value)> writeHold;
        // Plain write (coil/register) with a priority hint.
        std::function<SubmissionResult(quint16 address, bool value, CommandPriority priority)> writeCoil;
        std::function<SubmissionResult(quint16 address, quint16 value, CommandPriority priority)> writeRegister;
    };

    // Config. The former administrator-configurable 30-600 s reset timeout was
    // replaced by a fixed 200 ms completion delay (PLC-HMI-010 D3, spec §10.2);
    // no reset timing is configurable any more. The struct is retained as the
    // constructor's value parameter so existing wiring keeps one stable shape.
    struct Config {
    };

    // Result of a command submission (accepted vs rejected with a reason).
    struct CommandResult {
        bool accepted = false;
        QString reason;
    };

    // `nowMs` is injected for deterministic timeout tests (same pattern as
    // PulseStateMachine). Defaults to wall clock.
    explicit ControlCoordinator(PulseTransport transport,
                                Config config = Config(),
                                std::function<qint64()> nowMs = nullptr,
                                QObject *parent = nullptr);

    // --- session ------------------------------------------------------------
    void setRole(Role role);
    Role role() const { return m_role; }

    // Restricted-mode command gate (PLC-HMI-008 D1/D2). The composition root
    // installs the single LifecycleController::commandAllowed verdict here:
    // the callback returns an empty string while the command is allowed and a
    // non-empty operator-facing reason while restricted mode blocks it. Every
    // user-initiated command entry consults this gate before any
    // permission/interlock check, dispatch or PLC submission, so no duplicated
    // predicate exists. Internal safety/clear paths (logoutClear, stop/estop
    // convergence, shutdown clears) never consult the gate; Stop and EstopSet
    // stay allowed exactly as the verdict defines. When no gate is installed
    // (unit tests constructing a bare coordinator) every command is allowed.
    void setCommandGate(std::function<QString(Command)> gate);

    // --- commands (all return a structured result; effects arrive via signals)
    PermissionResult permission(Command cmd) const;
    // `targetWidth` is the AdjustWidth target; `address` is the manual coil for
    // Command::ManualCommand (PLC-HMI-011 D5). Both default to 0 so every
    // existing call site keeps compiling and the address-less verdict is
    // unchanged.
    InterlockResult interlock(Command cmd, const DeviceSnapshot &s,
                              quint16 targetWidth = 0, quint16 address = 0) const;

    // 复位 (spec §10.2). Admin only. Sends the M103 pulse; it does NOT start
    // homing (user decision 2026-09-21 — 回原点 is homeStart()).
    CommandResult reset();
    // 回原点 (user decision 2026-09-21). Admin only. Writes one sustained
    // M50=1 coil (never a pulse, never repeated); the PLC clears it and raises
    // M61/M9 when homing ends.
    CommandResult homeStart();
    // 配方调宽 (spec §10.3). Admin only. `targetWidth` is the raw D128 target
    // in 0.1 mm units (user decision 2026-09-21); no operator envelope is
    // applied — the only rule is > 0 (InterlockRules).
    //
    // Result determination (user decision 2026-09-22) uses the physical state
    // only, never the PLC's M44/M45 result flags: success = M34 (D103 bit4) 0
    // and D130 == the target written to D128; explicit failure = D110 == 10
    // (调宽定位超时) or M14 latched; otherwise "调宽未到位, 请检查限位与回原点
    // 状态".
    CommandResult adjustWidth(quint16 targetWidth);
    // 模式切换 (spec §10.1). Admin only.
    CommandResult setMode(bool autoMode);
    // 自动启动 (spec §10.4). Operator/Admin.
    CommandResult start();
    // 在线停止 (spec §10.5). Any user.
    CommandResult stop();
    // 软件急停 (spec §10.6). Set: any user; release: admin only.
    CommandResult estopSet();
    CommandResult estopRelease();
    // 手动命令 (spec §10.7). Admin only. M106/M107/M108 are hold commands
    // (press=1, release=0); M109 is a latched stop gate.
    CommandResult manualHold(quint16 address, bool pressed);
    CommandResult manualLatch(quint16 address, bool value);
    // 屏蔽 (spec §10.8). Admin only. M105/M42/M110/M111.
    CommandResult bypass(quint16 address, bool value);
    // 测试信号 (user decision 2026-09-22): one 100 ms pulse for one of the
    // simulated neighbouring-station signals (M114 前站进板 / M115 后站要板 /
    // M116 前站要板请求 / M117 后站出站请求). Admin only; the only gate is an
    // online link, because the signal must be injectable while the flow runs
    // (see InterlockRules::checkSimStationSignal). These coils are absent from
    // the current PLC program, so on an unchanged PLC the pulses are inert.
    // Each signal is single-flight and converges to exactly one terminal
    // ("已发送" on the correlated pulse completion, a failure on transport
    // error, timeout or link loss).
    CommandResult simStationPulse(quint16 address);
    // 注销/会话超时: try to clear M42/M106-M111; M100 is never touched
    // (spec §11.5, §13).
    void logoutClear();
    // Converges a still-pending logout clear to a visible failure. Used by the
    // composition root at shutdown after its bounded delivery/confirmation
    // window (REV-P0-1 D1(f)); no-op when no clear is pending.
    void failPendingLogoutClear(const QString &detail);

    // --- snapshot / submission result feed (from the gateway) ----------------
    void onSnapshot(const DeviceSnapshot &s);
    // Correlated terminal outcome of one accepted submission. Only a
    // completion whose request_id AND gateway_generation match a tracked
    // submission is consumed; stale, unknown and duplicate completions are
    // ignored (PLC-HMI-003 D5).
    void onSubmissionCompleted(const SubmissionCompletion &completion);
    void onConnectionChanged(bool online);

    // --- state --------------------------------------------------------------
    bool online() const { return m_online; }
    bool hasSnapshot() const { return m_snapshot.has_value(); }
    DeviceSnapshot snapshot() const { return m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData())); }
    bool resetInProgress() const { return m_resetPending; }
    bool homeStartInProgress() const { return m_homeStartCommandPending; }
    bool adjustInProgress() const { return m_adjustPhase != AdjustPhase::Idle; }
    bool startInProgress() const { return m_startPhase != StartPhase::Idle; }
    bool stopInProgress() const { return m_stopPhase != StopPhase::Idle; }
    // True while a logout/session-timeout clear is awaiting its correlated
    // write confirmations (REV-P0-1). The composition root uses this to bound
    // its shutdown delivery/confirmation window.
    bool logoutClearPending() const { return m_pendingClear.has_value(); }
    // Saved command context (spec §10.3 step 4): the D130 == target result
    // comparison. The start width/speed are no longer saved because the
    // defensive deadline is now the fixed PLC timeout (+3 s), not an
    // estimated-motion formula.
    std::optional<quint16> adjustTarget() const { return m_adjustTarget; }

    // The fixed reset completion delay from the M103 pulse submission
    // (PLC-HMI-010 D3, user decision U1: 200 ms). The reset also requires the
    // correlated M50 home-start write to complete before the success terminal.
    static constexpr qint64 kResetCompletionDelayMs = 200;
    // Defensive wait for the correlated M50 home-start completion
    // (PLC-HMI-011 D3): between the 2.1 s hold and the 120 s convergence bound
    // required by the approved contract; the 3 s class matches the other
    // transport-confirmation deadlines.
    static constexpr qint64 kHomeStartConfirmTimeoutMs = 3'000;

signals:
    // Command accepted and dispatched (waiting for PLC confirmation).
    void commandAccepted(Command cmd);
    // Command rejected by permission, interlock, duplicate/in-progress state or
    // transport submission failure (reason for UI display; never empty).
    void commandRejected(Command cmd, const QString &reason);
    // Command result confirmed by the PLC snapshot (never optimistic).
    void commandResult(Command cmd, bool ok, const QString &detail);
    // A command is waiting for PLC confirmation (UI shows 发送中/等待确认).
    void commandPending(Command cmd);
    // Additive (existing signal signatures unchanged): the human-readable
    // phase detail for the commandPending(cmd) state just emitted. Lets the
    // composition root project reset's 手动/回原点 phases and the
    // write/confirm phases of other flows into OperatorCommandStatus.
    void commandPendingDetail(Command cmd, const QString &detail);
    // Continuous-command clear requested (logout / session timeout).
    void continuousCleared();

private:
    enum class AdjustPhase { Idle, WaitTargetWrite, WaitResult };
    enum class StartPhase { Idle, WaitM3 };
    enum class StopPhase { Idle, WaitM3Clear };

    // One accepted hold/latch/bypass command awaiting snapshot confirmation.
    // Success is only emitted when a confirmed snapshot shows `value` at
    // `address`; otherwise the defensive timeout converges to failure.
    struct ManualConfirm {
        Command cmd = Command::ManualCommand;
        quint16 address = 0;
        bool value = false;
        qint64 deadlineMs = 0;
    };

    // One logout/session-timeout clear generation awaiting its correlated
    // write confirmations (REV-P0-1). The seven M42/M106-M111 writes are
    // registered in m_pendingSubmissions under Command::LogoutClear; this
    // keeps the completion count. Success may only be reported when every
    // registered write completion reports result==true; a failed completion,
    // link loss or the defensive deadline converges to a visible failure.
    struct PendingClear {
        int total = 0;
        int completed = 0;
        qint64 deadlineMs = 0;
    };

    // One accepted submission awaiting its correlated terminal completion.
    struct PendingSubmission {
        quint64 request_id = 0;
        quint64 gateway_generation = 0;
        Command cmd = Command::Count;
        PlcOperation operation = PlcOperation::WriteCoil;
        quint16 address = 0;
    };

    CommandResult gate(Command cmd, const DeviceSnapshot &s, quint16 targetWidth = 0,
                       quint16 address = 0);
    // Emits commandRejected(cmd, reason) and returns the structured result.
    CommandResult rejectCommand(Command cmd, const QString &reason);
    // Non-empty when the installed restricted-mode gate blocks `cmd`.
    QString blockedByRestrictedMode(Command cmd) const;
    // Submits and, when accepted, tracks the request identity for completion
    // correlation. False = the transport rejected the submission.
    bool submitCoil(Command cmd, quint16 address, bool value, CommandPriority priority);
    bool submitHold(Command cmd, quint16 address, bool value);
    bool submitRegister(Command cmd, quint16 address, quint16 value,
                        CommandPriority priority);
    bool submitPulse(Command cmd, quint16 address);
    // Shared body of simStationPulse(): gate, single-flight, pulse, pending.
    CommandResult pulseSimSignal(Command cmd, quint16 address);
    void trackSubmission(Command cmd, const SubmissionResult &result,
                         PlcOperation operation, quint16 address);
    void clearPendingSubmissions(Command cmd);
    void finishCommand(Command cmd, bool ok, const QString &detail);
    // Converges the pending logout clear to exactly one terminal. No-op when no
    // clear is pending (late/duplicate completions are ignored).
    void finishLogoutClear(bool ok, const QString &detail);

    // Emits commandPending(cmd) plus its phase detail.
    void emitPending(Command cmd);
    QString pendingDetail(Command cmd) const;

    bool hasManualConfirm(Command cmd, quint16 address, bool value) const;
    void confirmManualFromSnapshot(const DeviceSnapshot &s);
    void failAllManualConfirms(const QString &detail);
    // Converges exactly the pending confirmation for (cmd, address); a missing
    // entry is a late/duplicate outcome and is ignored (exactly one terminal).
    void failManualConfirm(Command cmd, quint16 address, const QString &detail);

    void onAdjustSnapshot(const DeviceSnapshot &s);
    void onStartSnapshot(const DeviceSnapshot &s);
    void onStopSnapshot(const DeviceSnapshot &s);
    // Converges the accepted reset from the snapshot feed: enforces the M103
    // pulse confirmation deadline and emits the single 复位完成 once the pulse
    // completion succeeded and the fixed 200 ms minimum elapsed. No-op when no
    // reset is pending (exactly one terminal).
    void onResetSnapshot();

    PulseTransport m_transport;
    Config m_cfg;
    std::function<qint64()> m_nowMs;
    // Restricted-mode verdict installed by the composition root; empty when no
    // restricted gate applies (see setCommandGate).
    std::function<QString(Command)> m_commandGate;
    Role m_role = Role::Anonymous;
    bool m_online = false;
    std::optional<DeviceSnapshot> m_snapshot;

    // Command lifecycle state.
    // Reset (user decision 2026-09-21): the M103 pulse is submitted and the
    // single terminal 复位完成 requires its correlated completion to succeed
    // and the fixed 200 ms minimum to have elapsed. Reset does not write M50 and
    // never uses snapshot M50/M61/M14/D110 as evidence; 回原点 is the separate
    // Command::HomeStart flow.
    bool m_resetPending = false;
    AdjustPhase m_adjustPhase = AdjustPhase::Idle;
    StartPhase m_startPhase = StartPhase::Idle;
    StopPhase m_stopPhase = StopPhase::Idle;
    std::optional<quint16> m_adjustTarget;
    // True once the correlated M43 pulse completion arrived (user decision
    // 2026-09-22): the adjust verdict is only evaluated from snapshots
    // delivered after the PLC has actually seen the command.
    bool m_adjustPulseDelivered = false;
    qint64 m_resetCompletionDeadlineMs = 0; // clock time of the fixed 200 ms boundary
    // M103 pulse correlated completion; the reset terminal may only follow it.
    bool m_resetPulseCompleted = false;
    // Defensive M103 pulse confirmation deadline (kHomeStartConfirmTimeoutMs
    // from the pulse submission); 0 while no reset is pending.
    qint64 m_homeStartDeadlineMs = 0;
    // Command::HomeStart is awaiting its M50 readback confirmation through
    // m_manualPending (same lifecycle class as the hold/latch/bypass commands).
    bool m_homeStartCommandPending = false;
    qint64 m_adjustDeadlineMs = 0; // fixed PLC width timeout + 3 s (spec §10.3)
    qint64 m_startDeadlineMs = 0;
    qint64 m_stopDeadlineMs = 0;
    bool m_adjustTimeoutArmed = false;
    bool m_startTimeoutArmed = false;
    bool m_stopTimeoutArmed = false;
    // Accepted submissions awaiting their correlated terminal completion.
    QVector<PendingSubmission> m_pendingSubmissions;
    // Mode switch (waits for M1/M2, spec §11.2).
    bool m_modePending = false;
    std::optional<bool> m_modeTarget;
    qint64 m_modeDeadlineMs = 0;
    // Estop confirmation (spec §8.4: until M0=1 or M100=1 readback).
    bool m_estopSetPending = false;
    bool m_estopReleasePending = false;
    // Defensive estop timeout (spec §13): the pending set/release flow fails
    // with a defined result if the snapshot never reflects the M100 write.
    qint64 m_estopDeadlineMs = 0;
    // Accepted hold/latch/bypass commands waiting for snapshot confirmation.
    QVector<ManualConfirm> m_manualPending;
    // 测试信号 M114-M117 (user decision 2026-09-22): one single-flight slot per
    // signal, indexed by simSignalIndex(), plus its defensive deadline. A pulse
    // whose correlated completion never arrives still converges visibly.
    static constexpr int kSimSignalCount = 4;
    bool m_simSignalPending[kSimSignalCount] = {false, false, false, false};
    qint64 m_simSignalDeadlineMs[kSimSignalCount] = {0, 0, 0, 0};
    // Pending logout/session-timeout clear generation (REV-P0-1). Only one
    // clear may be in flight; a duplicate logoutClear() while it is pending is
    // absorbed (no second generation, no second request-start).
    std::optional<PendingClear> m_pendingClear;
};

} // namespace hlm
