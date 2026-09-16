#include "application/control_coordinator.h"

#include <QDateTime>

#include <algorithm>

namespace hlm {

namespace {

// Protocol addresses (0-based, matching AddressTable). No bare addresses in
// UI/flow code (spec §8.2) — these are the coordinator's own flow constants.
constexpr quint16 kM1 = 1;    // manual mode
constexpr quint16 kM2 = 2;    // auto mode
constexpr quint16 kM42 = 42;  // belt continuous
constexpr quint16 kM43 = 43;  // width adjust command (pulse)
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
                                              quint16 targetWidth) const
{
    switch (cmd) {
    case Command::Reset: return InterlockRules::checkReset(s, m_online);
    case Command::AdjustWidth: return InterlockRules::checkAdjustWidth(s, m_online, targetWidth);
    case Command::ModeSwitch: return InterlockRules::checkModeSwitch(s, m_online);
    case Command::Start: return InterlockRules::checkStart(s, m_online);
    case Command::Stop: return InterlockRules::checkStop(s, m_online);
    case Command::EstopSet: return InterlockRules::checkEstopSet(s, m_online);
    case Command::EstopRelease: return InterlockRules::checkEstopRelease(s, m_online);
    case Command::ManualCommand: return InterlockRules::checkManualCommand(s, m_online);
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
                                                          quint16 targetWidth)
{
    const PermissionResult p = permission(cmd);
    if (!p.allowed)
        return {false, p.reason};
    const InterlockResult il = interlock(cmd, s, targetWidth);
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
    if (m_resetPhase != ResetPhase::Idle)
        return rejectCommand(Command::Reset,
                             QStringLiteral("复位已在进行中, 请等待当前复位结束"));
    // Reset selects manual mode through M104; a mode switch in flight would
    // share that coil and must never consume this flow's confirmation.
    if (m_modePending)
        return rejectCommand(Command::Reset,
                             QStringLiteral("模式切换已在进行中, 请稍后再试"));

    m_resetPhase = ResetPhase::WaitManual;
    m_resetHomingStarted = false;
    m_resetDeadlineMs = m_nowMs() + qint64(m_cfg.resetTimeoutSec) * 1000;
    m_resetTimeoutArmed = true;
    emit commandAccepted(Command::Reset);

    if (s.m1()) {
        // Already manual: pulse M103 directly (spec §10.2 step 2).
        if (submitPulse(Command::Reset, kM103)) {
            m_resetPhase = ResetPhase::Homing;
            emitPending(Command::Reset);
        } else {
            finishCommand(Command::Reset, false, QStringLiteral("M103 脉冲发送失败"));
        }
    } else {
        // Not manual: write M104=0 and wait for M1=1 (spec §10.2 step 1).
        // The pending phase is visible before the write result so the operator
        // sees the manual-switch phase (D4/OB-5).
        emitPending(Command::Reset);
        if (!submitCoil(Command::Reset, kM104, false, CommandPriority::Normal))
            finishCommand(Command::Reset, false, QStringLiteral("命令发送失败"));
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
    // Reset also drives M104; two flows sharing the coil must not interleave.
    if (m_resetPhase != ResetPhase::Idle)
        return rejectCommand(Command::ModeSwitch,
                             QStringLiteral("复位已在进行中, 无法切换模式"));

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
        CommandResult g = gate(Command::ManualCommand, s);
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
    CommandResult g = gate(Command::ManualCommand, s);
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
    // 注销/会话超时: try to clear M42/M106-M111 (spec §11.5, §13). M100 is
    // never touched; M105 模式选择保持不变 (spec §10.8). Every submission
    // result is inspected: the clear may only be reported as succeeded when
    // all seven writes were accepted (NF-03, no optimistic success). A missing
    // or rejecting transport converges to a visible communications-lost
    // failure instead of a fabricated success.
    bool allAccepted = static_cast<bool>(m_transport.writeCoil);
    if (m_transport.writeCoil) {
        allAccepted = m_transport.writeCoil(kM42, false, CommandPriority::Normal).accepted;
        for (quint16 a = kM106; a <= kM111; ++a)
            allAccepted = m_transport.writeCoil(a, false, CommandPriority::Normal).accepted
                          && allAccepted;
    }
    // 清零是原因, 保持命令的取消是结果: 被本次清零撤销的保持/锁存/屏蔽请求
    // 不可能再确认, 必须先以失败收敛, 否则会悬空到确认超时. Then report the
    // clear itself as a terminal result so restricted-mode entry has a visible,
    // converging command state (PLC-HMI-006 D3).
    failAllManualConfirms(QStringLiteral("注销清零: 保持命令已取消"));
    if (allAccepted)
        emit commandResult(Command::LogoutClear, true,
                           QStringLiteral("注销清零: 连续输出已清除"));
    else
        emit commandResult(Command::LogoutClear, false,
                           QStringLiteral("注销清零: 通讯中断, 连续输出清零未确认"));
    emit continuousCleared();
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
    if (m_resetTimeoutArmed && m_nowMs() >= m_resetDeadlineMs) {
        m_resetTimeoutArmed = false;
        finishCommand(Command::Reset, false,
                      QStringLiteral("回原点等待超时, 请检查设备"));
    }
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

    if (m_resetPhase != ResetPhase::Idle)
        onResetSnapshot(s);
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

    // A successful completion is not machine confirmation: the snapshot still
    // decides (spec §11.2 no optimistic success). Only a failed transfer
    // converges the correlated command here.
    if (completion.result)
        return;

    switch (pending.cmd) {
    case Command::Reset:
        // Only converge while the reset flow is still waiting; otherwise the
        // flow already converged (spec §13: never double-report).
        if (m_resetPhase == ResetPhase::Homing) {
            finishCommand(Command::Reset, false, QStringLiteral("M103 脉冲发送失败"));
        } else if (m_resetPhase != ResetPhase::Idle) {
            finishCommand(Command::Reset, false, QStringLiteral("写 M104 失败"));
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
    if (m_resetPhase != ResetPhase::Idle) {
        m_resetPhase = ResetPhase::Idle;
        m_resetTimeoutArmed = false;
        m_resetHomingStarted = false;
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
}

// --- flow snapshot handlers -------------------------------------------------

void ControlCoordinator::onResetSnapshot(const DeviceSnapshot &s)
{
    if (m_resetPhase == ResetPhase::WaitManual) {
        if (s.m1()) {
            if (submitPulse(Command::Reset, kM103)) {
                m_resetPhase = ResetPhase::Homing;
                emitPending(Command::Reset);
            } else {
                finishCommand(Command::Reset, false, QStringLiteral("M103 脉冲发送失败"));
            }
        }
        return;
    }
    if (m_resetPhase == ResetPhase::Homing) {
        // The flow enters Homing the moment the M103 pulse is sent, while the
        // pulse is still in flight. A snapshot read before the PLC processes
        // the rising edge still shows the pre-pulse state (spec §10.2 step 2).
        // Because reset does not require M14=0, a latched fault (the primary
        // use case) appears in that stale snapshot and must not be judged.
        // Only judge faults (or success) after M50=1 has been observed, i.e.
        // the home return actually started (spec §10.2 step 3 "监视 M50").
        if (s.m50())
            m_resetHomingStarted = true;
        if (m_resetHomingStarted) {
            // Fault check takes precedence over success (spec §10.2 step 6): a
            // latched fault (M14) or fault code must never be reported cleared.
            if (s.m14() || s.faultCode() != 0) {
                // PLC fault: keep the actual state, no optimistic success, no
                // fabricated fault code (spec §10.2 step 6).
                finishCommand(Command::Reset, false, QStringLiteral("回原点故障"));
                return;
            }
            // Success: home return started, then M61=1 and M50=0 (spec §10.2
            // step 4).
            if (s.m9() && !s.m50()) {
                finishCommand(Command::Reset, true, QStringLiteral("回原点完成"));
            }
        }
        // Before M50=1: keep waiting. If the M103 pulse was lost the machine
        // was already homed, M50 never rises and the flow converges to the
        // defensive timeout instead of a false success or premature fault
        // (spec §13: 不显示乐观成功, 每阶段收敛).
    }
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
        m_resetPhase = ResetPhase::Idle;
        m_resetTimeoutArmed = false;
        m_resetHomingStarted = false;
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
        return m_resetPhase == ResetPhase::WaitManual
            ? QStringLiteral("复位中: 正在切换到手动模式")
            : QStringLiteral("复位中: 回原点进行中");
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
        if (commandCoilValue(s, c.address) == c.value) {
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
    for (const ManualConfirm &c : pending)
        emit commandResult(c.cmd, false, detail);
}

void ControlCoordinator::setResetTimeoutSec(int sec)
{
    m_cfg.resetTimeoutSec = qBound(30, sec, 600); // spec §10.2: 30-600
}

} // namespace hlm
