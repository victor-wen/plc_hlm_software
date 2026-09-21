#include "application/control_coordinator.h"

#include <QDateTime>

#include <algorithm>

namespace hlm {

namespace {

// Protocol addresses (0-based, matching AddressTable). No bare addresses in
// UI/flow code (spec §8.2) — these are the coordinator's own flow constants.
constexpr quint16 kM2 = 2;    // auto mode
constexpr quint16 kM42 = 42;  // belt continuous
constexpr quint16 kM43 = 43;  // width adjust command (pulse)
constexpr quint16 kM50 = 50;  // home-start (sustained write, PLC-HMI-011)
constexpr quint16 kM100 = 100; // HMI estop request
constexpr quint16 kM101 = 101; // HMI start (pulse)
constexpr quint16 kM102 = 102; // HMI stop (pulse)
constexpr quint16 kM103 = 103; // HMI reset (pulse)
constexpr quint16 kM104 = 104; // auto mode select
constexpr quint16 kM105 = 105; // passthrough mode
constexpr quint16 kM106 = 106; // manual width forward (hold)
constexpr quint16 kM107 = 107; // manual width reverse (hold)
constexpr quint16 kM108 = 108; // manual belt jog (hold)
constexpr quint16 kM109 = 109; // manual stop gate (latch)
constexpr quint16 kM110 = 110; // light curtain bypass
constexpr quint16 kM111 = 111; // door bypass
constexpr quint16 kD128 = 128; // target width

// Fixed PLC width-adjust timeout (T6 K300, spec §10.3): the decoded PLC fails
// the adjustment 30 s after the M43 pulse, so the HMI defensive deadline is
// hmi_timeout = plc_timeout + 3 s = 33 s, never an estimated-motion formula
// that could expire before the PLC's own result (reviewer follow-up,
// PLC-HMI-005 amendment 4).
constexpr qint64 kPlcWidthAdjustTimeoutMs = 30'000;
constexpr qint64 kAdjustTimeoutMs = kPlcWidthAdjustTimeoutMs + 3'000;
// Start/stop defensive wait (spec §13: converge to the actual state).
constexpr qint64 kStartStopTimeoutMs = 10'000;
// Mode switch wait (spec §11.2: 模式切换等待 M1/M2).
constexpr qint64 kModeTimeoutMs = 5'000;
// Estop set/release wait (spec §13: every flow converges). If the PLC accepts
// the M100 write but never reflects M0/M100 in the snapshot, the pending state
// must not stay 待确认 forever — the flow fails with a defined result.
constexpr qint64 kEstopTimeoutMs = 5'000;
// Hold/latch/bypass confirmation wait (.ai/changes/PLC-HMI-001 D4): an
// accepted continuous command only succeeds when a confirmed snapshot shows
// the requested state; without confirmation it converges to failure.
constexpr qint64 kManualConfirmTimeoutMs = 3'000;
// Defensive logout/session-timeout clear deadline (REV-P0-1): the seven
// M42/M106-M111 writes must converge even if no correlated completion ever
// arrives. Same 3 s class as the manual confirmation timeout.
constexpr qint64 kLogoutClearTimeoutMs = 3'000;

// Reads the M42/M105-M111 readback bit that confirms a hold/latch/bypass
// command. Addresses are the coordinator's whitelisted ones (spec §10.7-§10.8).
bool commandCoilValue(const DeviceSnapshot &s, quint16 address)
{
    switch (address) {
    case kM42: return s.m42();
    case kM105: return s.m105();
    case kM106: return s.m106();
    case kM107: return s.m107();
    case kM108: return s.m108();
    case kM109: return s.m109();
    case kM110: return s.m110();
    case kM111: return s.m111();
    default: return false;
    }
}

QString manualConfirmDetail(Command cmd)
{
    return cmd == Command::Bypass ? QStringLiteral("屏蔽命令已确认")
                                  : QStringLiteral("手动命令已确认");
}

QString manualTimeoutDetail(Command cmd)
{
    return cmd == Command::Bypass
        ? QStringLiteral("屏蔽命令确认超时, 请检查设备")
        : QStringLiteral("手动命令确认超时, 请检查设备");
}

// REV-P1-2: a confirming bit may only be taken from the source block that
// actually carries it and only while that block's evidence is fresh/valid for
// the address. M42 is sourced from the fast block (quality encodes staleness,
// the age guard is defensive); M105-M111 from the command block. A stale or
// failed block therefore cannot confirm a just-issued command: the pending
// entry is left to the existing defensive timeout.
bool confirmationEvidenceFresh(const DeviceSnapshot &s, quint16 address)
{
    if (address == kM42)
        return s.fast_quality == DataQuality::Valid
            && s.fast_age_ms <= kFastStaleMs;
    return s.command_quality == DataQuality::Valid
        && s.command_age_ms <= kCommandStaleMs;
}

} // namespace

ControlCoordinator::ControlCoordinator(PulseTransport transport,
                                       Config config, std::function<qint64()> nowMs,
                                       QObject *parent)
    : QObject(parent)
    , m_transport(std::move(transport))
    , m_cfg(config)
    , m_nowMs(nowMs ? std::move(nowMs)
                    : []() { return QDateTime::currentMSecsSinceEpoch(); })
{
}

void ControlCoordinator::setRole(Role role)
{
    m_role = role;
}

void ControlCoordinator::setCommandGate(std::function<QString(Command)> gate)
{
    m_commandGate = std::move(gate);
}

QString ControlCoordinator::blockedByRestrictedMode(Command cmd) const
{
    return m_commandGate ? m_commandGate(cmd) : QString();
}

PermissionResult ControlCoordinator::permission(Command cmd) const
{
    return PermissionPolicy::check(m_role, cmd);
}

InterlockResult ControlCoordinator::interlock(Command cmd, const DeviceSnapshot &s,
                                              quint16 targetWidth, quint16 address) const
{
    switch (cmd) {
    case Command::Reset: return InterlockRules::checkReset(s, m_online);
    case Command::AdjustWidth: return InterlockRules::checkAdjustWidth(s, m_online, targetWidth);
    case Command::ModeSwitch: return InterlockRules::checkModeSwitch(s, m_online);
    case Command::Start: return InterlockRules::checkStart(s, m_online);
    case Command::Stop: return InterlockRules::checkStop(s, m_online);
    case Command::EstopSet: return InterlockRules::checkEstopSet(s, m_online);
    case Command::EstopRelease: return InterlockRules::checkEstopRelease(s, m_online);
    case Command::ManualCommand:
        return InterlockRules::checkManualCommand(s, m_online, address);
    case Command::Bypass: return InterlockRules::checkBypass(s, m_online);
    case Command::LogoutClear:
    case Command::ParameterChange:
    case Command::Count: break;
    }
    InterlockResult r;
    r.allowed = true;
    return r;
}

ControlCoordinator::CommandResult ControlCoordinator::gate(Command cmd,
                                                          const DeviceSnapshot &s,
                                                          quint16 targetWidth,
                                                          quint16 address)
{
    const PermissionResult p = permission(cmd);
    if (!p.allowed)
        return {false, p.reason};
    const InterlockResult il = interlock(cmd, s, targetWidth, address);
    if (!il.allowed)
        return {false, il.unmet.join(QStringLiteral("; "))};
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::rejectCommand(Command cmd,
                                                                    const QString &reason)
{
    emit commandRejected(cmd, reason);
    return {false, reason};
}

// --- command entry points ---------------------------------------------------

ControlCoordinator::CommandResult ControlCoordinator::reset()
{
    // Restricted-mode gate first: a blocked request is rejected visibly before
    // any dispatch and never reaches the transport/PLC (PLC-HMI-008 D1).
    if (const QString blocked = blockedByRestrictedMode(Command::Reset);
        !blocked.isEmpty())
        return rejectCommand(Command::Reset, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Reset, s);
    if (!g.accepted)
        return rejectCommand(Command::Reset, g.reason);
    if (m_resetPending)
        return rejectCommand(Command::Reset,
                             QStringLiteral("复位已在进行中, 请等待当前复位结束"));

    // Reset (PLC-HMI-011 D2): the M103 pulse is submitted first; the single
    // sustained M50=1 home-start write follows only from the pulse's successful
    // correlated completion (or the next snapshot), never before it. The
    // command converges to exactly one terminal 复位完成 once both correlated
    // completions succeeded and the fixed 200 ms minimum from this pulse
    // submission has elapsed; it never uses snapshot M50/M61/M14/D110.
    m_resetPending = true;
    m_resetCompletionDeadlineMs = m_nowMs() + kResetCompletionDelayMs;
    m_resetPulseCompleted = false;
    m_homeStartIssued = false;
    m_homeStartCompleted = false;
    // The defensive handshake deadline covers the whole reset: a pulse
    // completion that never arrives converges here just like a home-start
    // completion that never arrives (never pending indefinitely, D4).
    m_homeStartDeadlineMs = m_nowMs() + kHomeStartConfirmTimeoutMs;
    emit commandAccepted(Command::Reset);

    if (submitPulse(Command::Reset, kM103)) {
        emitPending(Command::Reset);
    } else {
        finishCommand(Command::Reset, false, QStringLiteral("M103 脉冲发送失败"));
    }
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::adjustWidth(quint16 targetWidth)
{
    if (const QString blocked = blockedByRestrictedMode(Command::AdjustWidth);
        !blocked.isEmpty())
        return rejectCommand(Command::AdjustWidth, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::AdjustWidth, s, targetWidth);
    if (!g.accepted)
        return rejectCommand(Command::AdjustWidth, g.reason);
    if (m_adjustPhase != AdjustPhase::Idle)
        return rejectCommand(Command::AdjustWidth,
                             QStringLiteral("调宽已在进行中, 请等待完成"));

    // 目标 == 当前: 显示"当前已是目标宽度", 不发送 M43 (spec §10.3 step 3).
    if (s.fieldValid(SnapshotField::CurrentWidth) && s.currentWidth() == targetWidth) {
        emit commandAccepted(Command::AdjustWidth);
        emit commandResult(Command::AdjustWidth, true,
                           QStringLiteral("当前已是目标宽度, 无需调宽"));
        return {true, QString()};
    }

    // Save the command context (spec §10.3 step 4): the saved target is the
    // authoritative result comparison (D130 == target); the removed estimated
    // deadline consumed the start width and speed, so they are no longer saved.
    m_adjustTarget = targetWidth;

    m_adjustPhase = AdjustPhase::WaitTargetWrite;
    emit commandAccepted(Command::AdjustWidth);
    if (!submitRegister(Command::AdjustWidth, kD128, targetWidth,
                        CommandPriority::Normal)) {
        finishCommand(Command::AdjustWidth, false, QStringLiteral("命令发送失败"));
        return {true, QString()};
    }
    // The submission was accepted with a request identity: pulse M43 now
    // (spec §10.3 step 4). A failed D128 completion still converges the flow
    // through onSubmissionCompleted; an asynchronous M43 failure converges
    // through its own completion. The result deadline replaces the short
    // write deadline once the pulse is in flight.
    m_adjustPhase = AdjustPhase::WaitResult;
    if (!submitPulse(Command::AdjustWidth, kM43)) {
        finishCommand(Command::AdjustWidth, false, QStringLiteral("M43 脉冲发送失败"));
        return {true, QString()};
    }
    // hmi_timeout = plc_timeout + 3 (spec §10.3) with the authoritative fixed
    // plc_timeout (T6 K300, 30 s): the defensive deadline must never expire
    // before the PLC's own width-adjust result.
    m_adjustDeadlineMs = m_nowMs() + kAdjustTimeoutMs;
    m_adjustTimeoutArmed = true;
    emitPending(Command::AdjustWidth);
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::setMode(bool autoMode)
{
    if (const QString blocked = blockedByRestrictedMode(Command::ModeSwitch);
        !blocked.isEmpty())
        return rejectCommand(Command::ModeSwitch, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::ModeSwitch, s);
    if (!g.accepted)
        return rejectCommand(Command::ModeSwitch, g.reason);
    if (m_modePending)
        return rejectCommand(Command::ModeSwitch,
                             QStringLiteral("模式切换已在进行中, 请稍后再试"));
    // Reset no longer drives M104 (PLC-HMI-010 D1): it sends the M103 pulse
    // only, so a mode switch and a reset no longer share a coil and may overlap.

    m_modePending = true;
    m_modeTarget = autoMode;
    m_modeDeadlineMs = m_nowMs() + kModeTimeoutMs;
    emit commandAccepted(Command::ModeSwitch);
    if (!submitCoil(Command::ModeSwitch, kM104, autoMode, CommandPriority::Normal))
        finishCommand(Command::ModeSwitch, false, QStringLiteral("命令发送失败"));
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::start()
{
    if (const QString blocked = blockedByRestrictedMode(Command::Start);
        !blocked.isEmpty())
        return rejectCommand(Command::Start, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Start, s);
    if (!g.accepted)
        return rejectCommand(Command::Start, g.reason);
    if (m_startPhase != StartPhase::Idle)
        return rejectCommand(Command::Start,
                             QStringLiteral("启动已在进行中, 请等待确认"));

    m_startPhase = StartPhase::WaitM3;
    m_startDeadlineMs = m_nowMs() + kStartStopTimeoutMs;
    m_startTimeoutArmed = true;
    emit commandAccepted(Command::Start);
    if (submitPulse(Command::Start, kM101)) {
        emitPending(Command::Start);
    } else {
        finishCommand(Command::Start, false, QStringLiteral("M101 脉冲发送失败"));
    }
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::stop()
{
    // The gate allows Stop while restricted (commandAllowed verdict); the
    // online-stop path must never be blocked by the restricted gate.
    if (const QString blocked = blockedByRestrictedMode(Command::Stop);
        !blocked.isEmpty())
        return rejectCommand(Command::Stop, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Stop, s);
    if (!g.accepted)
        return rejectCommand(Command::Stop, g.reason);
    if (m_stopPhase != StopPhase::Idle)
        return rejectCommand(Command::Stop,
                             QStringLiteral("停止已在进行中, 请等待确认"));

    m_stopPhase = StopPhase::WaitM3Clear;
    m_stopDeadlineMs = m_nowMs() + kStartStopTimeoutMs;
    m_stopTimeoutArmed = true;
    emit commandAccepted(Command::Stop);
    if (submitPulse(Command::Stop, kM102)) {
        emitPending(Command::Stop);
    } else {
        finishCommand(Command::Stop, false, QStringLiteral("M102 脉冲发送失败"));
    }
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::estopSet()
{
    // The gate allows EstopSet while restricted (commandAllowed verdict); the
    // software-estop path must never be blocked by the restricted gate.
    if (const QString blocked = blockedByRestrictedMode(Command::EstopSet);
        !blocked.isEmpty())
        return rejectCommand(Command::EstopSet, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::EstopSet, s);
    if (!g.accepted)
        return rejectCommand(Command::EstopSet, g.reason);
    if (m_estopSetPending)
        return rejectCommand(Command::EstopSet,
                             QStringLiteral("急停置位正在等待确认"));

    // M100=1 is idempotent and safe to retry; the UI shows 待确认 until the
    // snapshot shows M0=1 or M100=1 (spec §8.4).
    // The newest command wins: a set supersedes any in-flight release, so the
    // stale release pending state is cleared and its flow converges instead of
    // hanging on 待确认 forever (spec §13: every flow converges to a result).
    m_estopSetPending = true;
    m_estopDeadlineMs = m_nowMs() + kEstopTimeoutMs;
    if (m_estopReleasePending) {
        m_estopReleasePending = false;
        emit commandResult(Command::EstopRelease, false,
                           QStringLiteral("已被新的急停置位取代"));
    }
    emit commandAccepted(Command::EstopSet);
    if (!submitCoil(Command::EstopSet, kM100, true, CommandPriority::Safety))
        finishCommand(Command::EstopSet, false, QStringLiteral("命令发送失败"));
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::estopRelease()
{
    if (const QString blocked = blockedByRestrictedMode(Command::EstopRelease);
        !blocked.isEmpty())
        return rejectCommand(Command::EstopRelease, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::EstopRelease, s);
    if (!g.accepted)
        return rejectCommand(Command::EstopRelease, g.reason);
    if (m_estopReleasePending)
        return rejectCommand(Command::EstopRelease,
                             QStringLiteral("急停解除正在等待确认"));

    m_estopReleasePending = true;
    m_estopDeadlineMs = m_nowMs() + kEstopTimeoutMs;
    if (m_estopSetPending) {
        m_estopSetPending = false;
        emit commandResult(Command::EstopSet, false,
                           QStringLiteral("已被新的急停解除取代"));
    }
    emit commandAccepted(Command::EstopRelease);
    if (!submitCoil(Command::EstopRelease, kM100, false, CommandPriority::Safety))
        finishCommand(Command::EstopRelease, false, QStringLiteral("命令发送失败"));
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::manualHold(quint16 address, bool pressed)
{
    // Every user-initiated manual entry is gated first, press and release:
    // restricted mode blocks ManualCommand per the commandAllowed verdict, and
    // a blocked request stays visibly rejected instead of reaching the PLC.
    if (const QString blocked = blockedByRestrictedMode(Command::ManualCommand);
        !blocked.isEmpty())
        return rejectCommand(Command::ManualCommand, blocked);
    // Hold commands are only M106/M107/M108 (spec §10.7); reject others.
    if (address != kM106 && address != kM107 && address != kM108)
        return rejectCommand(Command::ManualCommand,
                             QStringLiteral("不支持的手动命令地址"));
    // Release (write 0) must bypass machine-state interlocks: if a fault/estop
    // latches while the button is held, the release must still be sent so the
    // continuous command clears immediately (spec §10.7 松开写 0, §13 立即请求
    // 清零). It must NOT bypass the permission check: 手动命令仅管理员 (spec
    // §10.7, §11.4 所有写命令统一校验).
    if (!pressed) {
        const PermissionResult p = permission(Command::ManualCommand);
        if (!p.allowed)
            return rejectCommand(Command::ManualCommand, p.reason);
    } else {
        const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
        CommandResult g = gate(Command::ManualCommand, s, 0, address);
        if (!g.accepted)
            return rejectCommand(Command::ManualCommand, g.reason);
    }
    // A duplicate press/release for the same address+value never returns
    // silently: the first request is still awaiting confirmation.
    if (hasManualConfirm(Command::ManualCommand, address, pressed))
        return rejectCommand(Command::ManualCommand,
                             QStringLiteral("手动命令正在等待确认"));
    if (!submitHold(Command::ManualCommand, address, pressed))
        return rejectCommand(Command::ManualCommand, QStringLiteral("命令发送失败"));

    // Accepted: pending until a confirmed snapshot shows the requested state
    // (no optimistic success, .ai/changes/PLC-HMI-001 D4).
    m_manualPending.append(
        {Command::ManualCommand, address, pressed, m_nowMs() + kManualConfirmTimeoutMs});
    emit commandAccepted(Command::ManualCommand);
    emitPending(Command::ManualCommand);
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::manualLatch(quint16 address, bool value)
{
    if (const QString blocked = blockedByRestrictedMode(Command::ManualCommand);
        !blocked.isEmpty())
        return rejectCommand(Command::ManualCommand, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::ManualCommand, s, 0, address);
    if (!g.accepted)
        return rejectCommand(Command::ManualCommand, g.reason);
    // The latched manual command is M109 (stop gate) only (spec §10.7).
    if (address != kM109)
        return rejectCommand(Command::ManualCommand,
                             QStringLiteral("不支持的手动命令地址"));
    if (hasManualConfirm(Command::ManualCommand, address, value))
        return rejectCommand(Command::ManualCommand,
                             QStringLiteral("手动命令正在等待确认"));
    if (!submitCoil(Command::ManualCommand, address, value, CommandPriority::Normal))
        return rejectCommand(Command::ManualCommand, QStringLiteral("命令发送失败"));

    m_manualPending.append(
        {Command::ManualCommand, address, value, m_nowMs() + kManualConfirmTimeoutMs});
    emit commandAccepted(Command::ManualCommand);
    emitPending(Command::ManualCommand);
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::bypass(quint16 address, bool value)
{
    if (const QString blocked = blockedByRestrictedMode(Command::Bypass);
        !blocked.isEmpty())
        return rejectCommand(Command::Bypass, blocked);
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Bypass, s);
    if (!g.accepted)
        return rejectCommand(Command::Bypass, g.reason);
    // 屏蔽 addresses are M105 (passthrough) and M42/M110/M111 (spec §10.8).
    if (address != kM42 && address != kM105 && address != kM110 && address != kM111)
        return rejectCommand(Command::Bypass, QStringLiteral("不支持的屏蔽地址"));
    if (hasManualConfirm(Command::Bypass, address, value))
        return rejectCommand(Command::Bypass,
                             QStringLiteral("屏蔽命令正在等待确认"));
    if (!submitCoil(Command::Bypass, address, value, CommandPriority::Normal))
        return rejectCommand(Command::Bypass, QStringLiteral("命令发送失败"));

    m_manualPending.append(
        {Command::Bypass, address, value, m_nowMs() + kManualConfirmTimeoutMs});
    emit commandAccepted(Command::Bypass);
    emitPending(Command::Bypass);
    return {true, QString()};
}

void ControlCoordinator::logoutClear()
{
    // 注销/会话超时: try to clear M42/M106-M111 plus the M50 home-start request
    // (spec §11.5, §13). M100 is never touched; M105 模式选择保持不变 (spec §10.8).
    //
    // REV-P0-1: enqueue acceptance is not machine confirmation. The clear is a
    // tracked command generation: it first becomes visibly Accepted/Pending,
    // its eight writes are registered for correlated completion through the
    // existing submission bookkeeping, and success may only be reported once
    // every registered completion reports result==true. Any rejected write, any
    // failed completion, link loss or the defensive deadline converges the
    // clear to exactly one visible failure — never a fabricated success.
    //
    // Duplicate guard: a clear already awaiting confirmation owns the single
    // lifecycle; a second request neither starts a new generation nor emits a
    // second request-start.
    if (m_pendingClear.has_value())
        return;

    // 清零是原因, 保持命令的取消是结果: 被本次清零撤销的保持/锁存/屏蔽请求
    // 不可能再确认, 必须先以失败收敛, 否则会悬空到确认超时.
    failAllManualConfirms(QStringLiteral("注销清零: 保持命令已取消"));

    // Immediate visible request-start (before any terminal result), mirroring
    // the other command flows. `total` is the fixed eight-write expectation so
    // a synchronously delivered completion can never complete the clear before
    // all eight submissions were accepted.
    m_pendingClear = PendingClear{8, 0, m_nowMs() + kLogoutClearTimeoutMs};
    emit commandAccepted(Command::LogoutClear);
    emitPending(Command::LogoutClear);

    // Register each accepted write for correlated completion. The addresses are
    // M42 and M106-M111, all written false, plus the M50=0 home-start clear
    // appended last (PLC-HMI-011 D7: the existing index expectations for
    // M42/M106-M111 keep their positions).
    int acceptedCount = 0;
    auto submitClearWrite = [this, &acceptedCount](quint16 address) {
        if (!m_transport.writeCoil)
            return;
        const SubmissionResult result =
            m_transport.writeCoil(address, false, CommandPriority::Normal);
        if (!result.accepted)
            return;
        ++acceptedCount;
        // A synchronous completion may already have converged the clear; a
        // late registration for it would only leak bookkeeping.
        if (m_pendingClear.has_value())
            trackSubmission(Command::LogoutClear, result, PlcOperation::WriteCoil,
                            address);
    };
    submitClearWrite(kM42);
    for (quint16 a = kM106; a <= kM111; ++a)
        submitClearWrite(a);
    submitClearWrite(kM50); // appended last (PLC-HMI-011 D7)

    if (m_pendingClear.has_value()) {
        if (acceptedCount != m_pendingClear->total) {
            // A missing or rejecting transport can never confirm: visible
            // communications-lost failure immediately.
            finishLogoutClear(
                false, QStringLiteral("注销清零: 通讯中断, 连续输出清零未确认"));
        } else if (m_pendingClear->completed >= m_pendingClear->total) {
            // All completions were already delivered synchronously.
            finishLogoutClear(true, QStringLiteral("注销清零: 连续输出已清除"));
        }
    }
    // The hold-intent release stays immediate and is emitted exactly once per
    // clear request, independent of the terminal outcome (consumers:
    // LifecycleController / MainWindow::clearHoldIntents).
    emit continuousCleared();
}

void ControlCoordinator::finishLogoutClear(bool ok, const QString &detail)
{
    if (!m_pendingClear.has_value())
        return; // already converged: late/duplicate results are ignored
    m_pendingClear.reset();
    // No further completion may converge this generation (exactly one terminal).
    clearPendingSubmissions(Command::LogoutClear);
    emit commandResult(Command::LogoutClear, ok, detail);
}

// --- gateway feed -----------------------------------------------------------

void ControlCoordinator::onSnapshot(const DeviceSnapshot &s)
{
    m_snapshot = s;
    // A snapshot carrying connected=true implies the link is online (the
    // gateway only publishes full snapshots while online).
    if (s.connected())
        m_online = true;

    // Timeout convergence first (spec §13: 写结果不确定 -> 向安全状态收敛).
    if (m_adjustTimeoutArmed && m_nowMs() >= m_adjustDeadlineMs) {
        m_adjustTimeoutArmed = false;
        finishCommand(Command::AdjustWidth, false,
                      QStringLiteral("调宽等待超时, 请检查设备"));
    }
    if (m_startTimeoutArmed && m_nowMs() >= m_startDeadlineMs) {
        m_startTimeoutArmed = false;
        finishCommand(Command::Start, false,
                      QStringLiteral("启动超时, 请检查互锁条件"));
    }
    if (m_stopTimeoutArmed && m_nowMs() >= m_stopDeadlineMs) {
        m_stopTimeoutArmed = false;
        finishCommand(Command::Stop, false,
                      QStringLiteral("停止超时, 请检查设备"));
    }
    // Estop set/release defensive timeout (spec §13): if the PLC accepted the
    // M100 write but never reflects M0/M100 in the snapshot, the flow must
    // still converge to a defined failure instead of 待确认 forever.
    if (m_estopSetPending && m_nowMs() >= m_estopDeadlineMs) {
        m_estopSetPending = false;
        finishCommand(Command::EstopSet, false,
                      QStringLiteral("急停置位超时, 请检查设备"));
    }
    if (m_estopReleasePending && m_nowMs() >= m_estopDeadlineMs) {
        m_estopReleasePending = false;
        finishCommand(Command::EstopRelease, false,
                      QStringLiteral("急停解除超时, 请检查设备"));
    }

    // Hold/latch/bypass confirmation and defensive timeout (D4): the terminal
    // success only follows a snapshot that shows the requested machine state.
    confirmManualFromSnapshot(s);

    // Logout-clear defensive deadline (REV-P0-1): a correlated completion that
    // never arrives still converges the clear to one visible failure. The
    // check rides the snapshot feed (the same clock/tick a manual confirm
    // timeout uses).
    if (m_pendingClear.has_value() && m_nowMs() >= m_pendingClear->deadlineMs) {
        finishLogoutClear(
            false, QStringLiteral("注销清零: 确认超时, 连续输出清零未确认"));
    }

    // Estop confirmation (spec §8.4: 直到 M0=1 或读回 M100=1).
    if (m_estopSetPending && (s.m0() || s.m100())) {
        m_estopSetPending = false;
        emit commandResult(Command::EstopSet, true, QStringLiteral("急停已置位"));
    }
    // Estop release confirms when either the physical estop is released
    // (M0=0) or the M100 readback shows the release write took effect
    // (spec §8.4, §10.6: 解除请求成功 ≠ 设备可运行, which is decided later by
    // M0/M14/M60). Without the M100 readback a stuck physical estop would hold
    // M0=1 and the release would hang on 待确认 forever.
    //
    // Wording (contract invariant 464, PLC-HMI-001 D5): clearing M100 only
    // releases the software request; while the physical estop still holds
    // M0=1 the terminal detail must say so and must not claim a full release.
    if (m_estopReleasePending && (!s.m0() || !s.m100())) {
        m_estopReleasePending = false;
        if (s.m0()) {
            emit commandResult(
                Command::EstopRelease, true,
                QStringLiteral("软件急停请求已解除, 实体急停 (M0) 仍然有效"));
        } else {
            emit commandResult(Command::EstopRelease, true,
                               QStringLiteral("急停已解除"));
        }
    }

    // Mode switch waits for M1/M2 (spec §11.2).
    if (m_modePending) {
        const bool target = *m_modeTarget;
        if ((target && s.m2()) || (!target && s.m1())) {
            m_modePending = false;
            emit commandResult(Command::ModeSwitch, true,
                               target ? QStringLiteral("已切换至自动模式")
                                      : QStringLiteral("已切换至手动模式"));
        } else if (m_nowMs() >= m_modeDeadlineMs) {
            m_modePending = false;
            emit commandResult(Command::ModeSwitch, false,
                               QStringLiteral("模式切换超时"));
        }
    }

    // Reset handshake (PLC-HMI-011 D2/D3): issue the single M50 write when the
    // pulse completion arrived without it, converge the M50 confirmation
    // deadline, and emit the single 复位完成 once both correlated completions
    // succeeded and the fixed 200 ms minimum elapsed. Evaluated on the snapshot
    // feed (the same clock/tick the other defensive deadlines use); no
    // M50/M61/M14/D110 readback participates in the result (D2).
    onResetSnapshot();

    if (m_adjustPhase != AdjustPhase::Idle)
        onAdjustSnapshot(s);
    if (m_startPhase != StartPhase::Idle)
        onStartSnapshot(s);
    if (m_stopPhase != StopPhase::Idle)
        onStopSnapshot(s);
}

void ControlCoordinator::onSubmissionCompleted(const SubmissionCompletion &completion)
{
    // Correlate by request identity + gateway generation only (contract
    // invariant: never by address or FIFO position). An unknown request id, a
    // stale/obsolete generation or a duplicate completion is ignored.
    int index = -1;
    for (int i = 0; i < m_pendingSubmissions.size(); ++i) {
        const PendingSubmission &p = m_pendingSubmissions.at(i);
        if (p.request_id == completion.request_id
            && p.gateway_generation == completion.gateway_generation) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;
    const PendingSubmission pending = m_pendingSubmissions.takeAt(index);

    // REV-P0-1: logout-clear completions only update the clear bookkeeping.
    // Success is emitted when every registered completion reported result==true;
    // the first failed completion converges the clear to one visible failure.
    if (pending.cmd == Command::LogoutClear) {
        if (!m_pendingClear.has_value())
            return; // already converged: late/duplicate completion ignored
        if (!completion.result) {
            finishLogoutClear(
                false,
                completion.error.trimmed().isEmpty()
                    ? QStringLiteral("注销清零: 通讯中断, 连续输出清零未确认")
                    : QStringLiteral("注销清零: 连续输出清零未确认 (%1)")
                          .arg(completion.error));
            return;
        }
        ++m_pendingClear->completed;
        if (m_pendingClear->completed >= m_pendingClear->total)
            finishLogoutClear(true, QStringLiteral("注销清零: 连续输出已清除"));
        return;
    }

    // A successful completion is not machine confirmation: the snapshot still
    // decides (spec §11.2 no optimistic success). Only a failed transfer
    // converges the correlated command here.
    if (completion.result) {
        // PLC-HMI-011 D2: the successful M103 pulse completion is the trigger
        // for the single sustained M50=1 home-start write. The M50 write's own
        // successful completion is only bookkeeping; the terminal success is
        // decided on the snapshot feed once both outcomes are known.
        if (pending.cmd == Command::Reset) {
            if (pending.operation == PlcOperation::Pulse) {
                m_resetPulseCompleted = true;
                issueHomeStart();
            } else {
                m_homeStartCompleted = true;
            }
        }
        return;
    }

    switch (pending.cmd) {
    case Command::Reset:
        if (!m_resetPending)
            break;
        if (pending.operation == PlcOperation::Pulse) {
            // The M103 pulse failed: the home-start write is never attempted
            // (PLC-HMI-011 D4) and the command converges visibly.
            finishCommand(Command::Reset, false, QStringLiteral("M103 脉冲发送失败"));
        } else {
            // The M50 write failed: one visible non-success terminal, never
            // masked by the fixed delay (PLC-HMI-011 D4).
            finishCommand(Command::Reset, false, QStringLiteral("M50 回原点启动写入失败"));
        }
        break;
    case Command::AdjustWidth:
        if (pending.operation == PlcOperation::Pulse) {
            // A failed M43 pulse means the width adjustment never started.
            if (m_adjustPhase != AdjustPhase::Idle) {
                finishCommand(Command::AdjustWidth, false,
                              QStringLiteral("M43 脉冲发送失败"));
            }
        } else if (m_adjustPhase != AdjustPhase::Idle) {
            finishCommand(Command::AdjustWidth, false, QStringLiteral("写 D128 失败"));
        }
        break;
    case Command::EstopSet:
        if (m_estopSetPending)
            finishCommand(Command::EstopSet, false, QStringLiteral("写 M100 失败"));
        break;
    case Command::EstopRelease:
        if (m_estopReleasePending)
            finishCommand(Command::EstopRelease, false, QStringLiteral("写 M100 失败"));
        break;
    case Command::ModeSwitch:
        // A failed M104 write surfaces as a write failure, not 模式切换超时.
        if (m_modePending)
            finishCommand(Command::ModeSwitch, false, QStringLiteral("写 M104 失败"));
        break;
    case Command::ManualCommand:
    case Command::Bypass: {
        // Fail exactly the hold/latch/bypass request that owns this identity.
        for (int i = 0; i < m_manualPending.size(); ++i) {
            const ManualConfirm &c = m_manualPending.at(i);
            if (c.cmd == pending.cmd && c.address == pending.address) {
                m_manualPending.removeAt(i);
                emit commandResult(pending.cmd, false,
                                   QStringLiteral("命令发送失败"));
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}

void ControlCoordinator::onConnectionChanged(bool online)
{
    m_online = online;
    if (online)
        return;
    // Offline: abort active flows, never optimistic, never replay (spec §13).
    if (m_resetPending) {
        m_resetPending = false;
        // The handshake state dies with the generation: a late M103/M50
        // completion for the lost link can never advance a new reset.
        m_resetPulseCompleted = false;
        m_homeStartIssued = false;
        m_homeStartCompleted = false;
        m_homeStartDeadlineMs = 0;
        emit commandResult(Command::Reset, false, QStringLiteral("通讯中断"));
    }
    if (m_adjustPhase != AdjustPhase::Idle) {
        m_adjustPhase = AdjustPhase::Idle;
        m_adjustTimeoutArmed = false;
        m_adjustTarget.reset();
        emit commandResult(Command::AdjustWidth, false, QStringLiteral("通讯中断"));
    }
    if (m_startPhase != StartPhase::Idle) {
        m_startPhase = StartPhase::Idle;
        m_startTimeoutArmed = false;
        emit commandResult(Command::Start, false, QStringLiteral("通讯中断"));
    }
    if (m_stopPhase != StopPhase::Idle) {
        m_stopPhase = StopPhase::Idle;
        m_stopTimeoutArmed = false;
        emit commandResult(Command::Stop, false, QStringLiteral("通讯中断"));
    }
    // Every in-flight submission identity is dropped: a late completion for
    // the old generation must never converge a command of the new one.
    m_pendingSubmissions.clear();
    if (m_modePending) {
        m_modePending = false;
        emit commandResult(Command::ModeSwitch, false, QStringLiteral("通讯中断"));
    }
    // Estop set/release converge on link loss: M100 is idempotent, but the
    // pending flow must not stay 待确认 forever (D3/OB-3); the operator sees
    // communications-lost and can re-issue after reconnect.
    if (m_estopSetPending) {
        m_estopSetPending = false;
        finishCommand(Command::EstopSet, false, QStringLiteral("通讯中断"));
    }
    if (m_estopReleasePending) {
        m_estopReleasePending = false;
        finishCommand(Command::EstopRelease, false, QStringLiteral("通讯中断"));
    }
    // Accepted hold/latch/bypass commands converge too (never pending forever).
    failAllManualConfirms(QStringLiteral("通讯中断"));
    // A pending logout/session-timeout clear converges on link loss as well:
    // its correlated write completions can no longer confirm the clear. The
    // error text carries 通讯中断 so the projection reports CommunicationsLost
    // (PLC-HMI-006 D3 / NF-03).
    if (m_pendingClear.has_value()) {
        // m_pendingSubmissions was already cleared above: converge directly.
        finishLogoutClear(
            false, QStringLiteral("注销清零: 通讯中断, 连续输出清零未确认"));
    }
}

// --- flow snapshot handlers -------------------------------------------------

void ControlCoordinator::issueHomeStart()
{
    if (!m_resetPending || !m_resetPulseCompleted || m_homeStartIssued)
        return;
    // Exactly one sustained coil write, never a pulse (PLC-HMI-011 D2). The
    // write is submitted at the current priority class of the other flows.
    if (submitCoil(Command::Reset, kM50, true, CommandPriority::Normal)) {
        m_homeStartIssued = true;
    } else {
        // Submission rejected: one visible non-success terminal with a
        // non-empty detail, never masked by the fixed delay (D4).
        finishCommand(Command::Reset, false, QStringLiteral("M50 回原点启动写入失败"));
    }
}

void ControlCoordinator::onResetSnapshot()
{
    if (!m_resetPending)
        return;
    // A pulse completion may have been consumed before the write was issued
    // (the frozen helper drives same-time snapshots): issue it here as well.
    issueHomeStart();
    if (!m_resetPending)
        return; // a rejected M50 submission just converged the reset
    if (!m_homeStartIssued) {
        // The pulse completion is still outstanding: wait for it, but not
        // indefinitely — a pulse whose correlated completion never arrives must
        // still converge to one visible non-success terminal (D4).
        if (m_nowMs() >= m_homeStartDeadlineMs)
            finishCommand(Command::Reset, false,
                          QStringLiteral("M103 脉冲确认超时, 请检查设备"));
        return;
    }
    if (!m_homeStartCompleted && m_nowMs() >= m_homeStartDeadlineMs) {
        finishCommand(Command::Reset, false,
                      QStringLiteral("M50 回原点启动确认超时, 请检查设备"));
        return;
    }
    // Both correlated transport outcomes succeeded and the fixed minimum delay
    // from the M103 submission elapsed: exactly one 复位完成 (PLC-HMI-011 D3).
    if (m_homeStartCompleted && m_nowMs() >= m_resetCompletionDeadlineMs)
        finishCommand(Command::Reset, true, QStringLiteral("复位完成"));
}

void ControlCoordinator::onAdjustSnapshot(const DeviceSnapshot &s)
{
    if (m_adjustPhase != AdjustPhase::WaitResult)
        return;
    if (s.m34())
        return; // still adjusting
    // Only accept the result from a snapshot after the pulse (spec §10.3
    // step 5). Success: M34=0, M44=1, M45=0, D130 == saved target.
    if (s.m44() && !s.m45()
        && s.fieldValid(SnapshotField::CurrentWidth)
        && s.currentWidth() == m_adjustTarget.value_or(0)) {
        finishCommand(Command::AdjustWidth, true, QStringLiteral("调宽完成"));
    } else if (!s.m44() && s.m45()) {
        finishCommand(Command::AdjustWidth, false, QStringLiteral("调宽失败"));
    }
    // Transient idle (M34=0, M44=0, M45=0): keep waiting for the PLC result.
}

void ControlCoordinator::onStartSnapshot(const DeviceSnapshot &s)
{
    if (s.m3()) {
        finishCommand(Command::Start, true, QStringLiteral("已启动"));
    }
}

void ControlCoordinator::onStopSnapshot(const DeviceSnapshot &s)
{
    if (!s.m3()) {
        finishCommand(Command::Stop, true, QStringLiteral("已停止"));
    }
}

// --- helpers ----------------------------------------------------------------

bool ControlCoordinator::submitCoil(Command cmd, quint16 address, bool value,
                                    CommandPriority priority)
{
    if (!m_transport.writeCoil)
        return false;
    const SubmissionResult result = m_transport.writeCoil(address, value, priority);
    if (!result.accepted)
        return false;
    trackSubmission(cmd, result, PlcOperation::WriteCoil, address);
    return true;
}

bool ControlCoordinator::submitHold(Command cmd, quint16 address, bool value)
{
    if (!m_transport.writeHold)
        return false;
    const SubmissionResult result = m_transport.writeHold(address, value);
    if (!result.accepted)
        return false;
    trackSubmission(cmd, result, PlcOperation::WriteCoil, address);
    return true;
}

bool ControlCoordinator::submitRegister(Command cmd, quint16 address, quint16 value,
                                        CommandPriority priority)
{
    if (!m_transport.writeRegister)
        return false;
    const SubmissionResult result =
        m_transport.writeRegister(address, value, priority);
    if (!result.accepted)
        return false;
    trackSubmission(cmd, result, PlcOperation::WriteRegister, address);
    return true;
}

bool ControlCoordinator::submitPulse(Command cmd, quint16 address)
{
    if (!m_transport.startPulse)
        return false;
    const SubmissionResult result = m_transport.startPulse(address);
    if (!result.accepted)
        return false;
    trackSubmission(cmd, result, PlcOperation::Pulse, address);
    return true;
}

void ControlCoordinator::trackSubmission(Command cmd, const SubmissionResult &result,
                                         PlcOperation operation, quint16 address)
{
    m_pendingSubmissions.append(
        {result.request_id, result.gateway_generation, cmd, operation, address});
}

void ControlCoordinator::clearPendingSubmissions(Command cmd)
{
    for (int i = m_pendingSubmissions.size() - 1; i >= 0; --i) {
        if (m_pendingSubmissions.at(i).cmd == cmd)
            m_pendingSubmissions.removeAt(i);
    }
}

void ControlCoordinator::finishCommand(Command cmd, bool ok, const QString &detail)
{
    switch (cmd) {
    case Command::Reset:
        m_resetPending = false;
        // The handshake state belongs to the converged generation: a new reset
        // starts clean, and a late completion can never advance it.
        m_resetPulseCompleted = false;
        m_homeStartIssued = false;
        m_homeStartCompleted = false;
        m_homeStartDeadlineMs = 0;
        break;
    case Command::AdjustWidth:
        m_adjustPhase = AdjustPhase::Idle;
        m_adjustTimeoutArmed = false;
        m_adjustTarget.reset();
        break;
    case Command::Start:
        m_startPhase = StartPhase::Idle;
        m_startTimeoutArmed = false;
        break;
    case Command::Stop:
        m_stopPhase = StopPhase::Idle;
        m_stopTimeoutArmed = false;
        break;
    case Command::EstopSet:
        m_estopSetPending = false;
        break;
    case Command::EstopRelease:
        m_estopReleasePending = false;
        break;
    case Command::ModeSwitch:
        m_modePending = false;
        break;
    default:
        break;
    }
    // The command converged: its in-flight submission identities can no longer
    // produce a terminal outcome for it (late completions are ignored).
    clearPendingSubmissions(cmd);
    emit commandResult(cmd, ok, detail);
}

void ControlCoordinator::emitPending(Command cmd)
{
    emit commandPending(cmd);
    emit commandPendingDetail(cmd, pendingDetail(cmd));
}

QString ControlCoordinator::pendingDetail(Command cmd) const
{
    switch (cmd) {
    case Command::Reset:
        // Fire-and-confirm-by-fixed-delay (PLC-HMI-010 D1/D5): a single honest
        // pending detail — no manual-switch and no homing step, neither of
        // which the request performs.
        return QStringLiteral("复位中: 正在发送复位信号");
    case Command::AdjustWidth:
        return m_adjustPhase == AdjustPhase::WaitTargetWrite
            ? QStringLiteral("调宽: 正在写入目标宽度")
            : QStringLiteral("调宽: 等待 PLC 结果");
    case Command::Start:
        return QStringLiteral("启动中: 等待设备运行确认");
    case Command::Stop:
        return QStringLiteral("停止中: 等待设备停止确认");
    case Command::ModeSwitch:
        return QStringLiteral("模式切换中: 等待 PLC 确认");
    case Command::EstopSet:
        return QStringLiteral("急停置位中: 等待 PLC 确认");
    case Command::EstopRelease:
        return QStringLiteral("急停解除中: 等待 PLC 确认");
    case Command::ManualCommand:
        return QStringLiteral("手动命令已发送: 等待 PLC 确认");
    case Command::Bypass:
        return QStringLiteral("屏蔽命令已发送: 等待 PLC 确认");
    case Command::LogoutClear:
        return QStringLiteral("注销清零: 已提交, 等待 PLC 确认");
    default:
        return QStringLiteral("命令已发送: 等待 PLC 确认");
    }
}

bool ControlCoordinator::hasManualConfirm(Command cmd, quint16 address, bool value) const
{
    for (const ManualConfirm &c : m_manualPending) {
        if (c.cmd == cmd && c.address == address && c.value == value)
            return true;
    }
    return false;
}

void ControlCoordinator::confirmManualFromSnapshot(const DeviceSnapshot &s)
{
    for (int i = 0; i < m_manualPending.size();) {
        const ManualConfirm c = m_manualPending.at(i);
        // REV-P1-2: only fresh/valid evidence for the confirmed address may
        // confirm. A block that is stale or failed does not confirm; the entry
        // stays pending and the defensive timeout below converges it.
        if (confirmationEvidenceFresh(s, c.address)
            && commandCoilValue(s, c.address) == c.value) {
            m_manualPending.removeAt(i);
            emit commandResult(c.cmd, true, manualConfirmDetail(c.cmd));
            continue;
        }
        if (m_nowMs() >= c.deadlineMs) {
            m_manualPending.removeAt(i);
            emit commandResult(c.cmd, false, manualTimeoutDetail(c.cmd));
            continue;
        }
        ++i;
    }
}

void ControlCoordinator::failAllManualConfirms(const QString &detail)
{
    const QVector<ManualConfirm> pending = m_manualPending;
    m_manualPending.clear();
    // A cancelled confirmation must not leave dangling submission bookkeeping:
    // drop every submission identity owned by the cancelled command so a late
    // completion can never converge it a second time (OB-7: exactly one
    // terminal per generation).
    for (const ManualConfirm &c : pending)
        clearPendingSubmissions(c.cmd);
    for (const ManualConfirm &c : pending)
        emit commandResult(c.cmd, false, detail);
}

void ControlCoordinator::failPendingLogoutClear(const QString &detail)
{
    finishLogoutClear(
        false, detail.trimmed().isEmpty()
                   ? QStringLiteral("注销清零: 通讯中断, 连续输出清零未确认")
                   : detail);
}

} // namespace hlm
