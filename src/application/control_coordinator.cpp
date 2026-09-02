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

// Start/stop defensive wait (spec §13: converge to the actual state).
constexpr qint64 kStartStopTimeoutMs = 10'000;
// Mode switch wait (spec §11.2: 模式切换等待 M1/M2).
constexpr qint64 kModeTimeoutMs = 5'000;
// D128 target-width write wait (spec §13: the adjust flow must never hang —
// even if a concurrent safety write overwrites the pending write and the D128
// writeCompleted is dropped, the flow still converges to a defined failure).
constexpr qint64 kAdjustWriteTimeoutMs = 5'000;

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

// --- command entry points ---------------------------------------------------

ControlCoordinator::CommandResult ControlCoordinator::reset()
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Reset, s);
    if (!g.accepted) {
        emit commandRejected(Command::Reset, g.reason);
        return g;
    }
    if (m_resetPhase != ResetPhase::Idle)
        return {false, QStringLiteral("复位已在进行中")};

    m_resetPhase = ResetPhase::WaitManual;
    m_resetHomingStarted = false;
    m_resetDeadlineMs = m_nowMs() + qint64(m_cfg.resetTimeoutSec) * 1000;
    m_resetTimeoutArmed = true;
    emit commandAccepted(Command::Reset);

    if (s.m1()) {
        // Already manual: pulse M103 directly (spec §10.2 step 2).
        if (m_transport.startPulse && m_transport.startPulse(kM103)) {
            m_resetPhase = ResetPhase::Homing;
            emit commandPending(Command::Reset);
        } else {
            finishCommand(Command::Reset, false, QStringLiteral("M103 脉冲发送失败"));
        }
    } else {
        // Not manual: write M104=0 and wait for M1=1 (spec §10.2 step 1).
        beginWrite(Command::Reset, kM104, false, CommandPriority::Normal);
    }
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::adjustWidth(quint16 targetWidth)
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::AdjustWidth, s, targetWidth);
    if (!g.accepted) {
        emit commandRejected(Command::AdjustWidth, g.reason);
        return g;
    }
    if (m_adjustPhase != AdjustPhase::Idle)
        return {false, QStringLiteral("调宽已在进行中")};

    // 目标 == 当前: 显示"当前已是目标宽度", 不发送 M43 (spec §10.3 step 3).
    if (s.fieldValid(SnapshotField::CurrentWidth) && s.currentWidth() == targetWidth) {
        emit commandAccepted(Command::AdjustWidth);
        emit commandResult(Command::AdjustWidth, true,
                           QStringLiteral("当前已是目标宽度, 无需调宽"));
        return {true, QString()};
    }

    // Save the command context (spec §10.3 step 4).
    m_adjustTarget = targetWidth;
    m_adjustStartWidth = s.fieldValid(SnapshotField::CurrentWidth) ? s.currentWidth() : 0;
    m_adjustSpeed = s.fieldValid(SnapshotField::WidthSpeed) ? s.widthSpeed() : 0;

    m_adjustPhase = AdjustPhase::WaitTargetWrite;
    // Arm a defensive timeout for the D128 write (spec §13): if a concurrent
    // safety command overwrites m_pendingWriteCmd and this writeCompleted is
    // dropped, the flow still converges to a defined failure instead of
    // hanging forever. The result timeout replaces this once M43 is pulsed.
    m_adjustDeadlineMs = m_nowMs() + kAdjustWriteTimeoutMs;
    m_adjustTimeoutArmed = true;
    emit commandAccepted(Command::AdjustWidth);
    beginWriteReg(Command::AdjustWidth, kD128, targetWidth, CommandPriority::Normal);
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::setMode(bool autoMode)
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::ModeSwitch, s);
    if (!g.accepted) {
        emit commandRejected(Command::ModeSwitch, g.reason);
        return g;
    }
    if (m_modePending)
        return {false, QStringLiteral("模式切换已在进行中")};

    m_modePending = true;
    m_modeTarget = autoMode;
    m_modeDeadlineMs = m_nowMs() + kModeTimeoutMs;
    emit commandAccepted(Command::ModeSwitch);
    beginWrite(Command::ModeSwitch, kM104, autoMode, CommandPriority::Normal);
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::start()
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Start, s);
    if (!g.accepted) {
        emit commandRejected(Command::Start, g.reason);
        return g;
    }
    if (m_startPhase != StartPhase::Idle)
        return {false, QStringLiteral("启动已在进行中")};

    m_startPhase = StartPhase::WaitM3;
    m_startDeadlineMs = m_nowMs() + kStartStopTimeoutMs;
    m_startTimeoutArmed = true;
    emit commandAccepted(Command::Start);
    if (m_transport.startPulse && m_transport.startPulse(kM101)) {
        emit commandPending(Command::Start);
    } else {
        finishCommand(Command::Start, false, QStringLiteral("M101 脉冲发送失败"));
    }
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::stop()
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Stop, s);
    if (!g.accepted) {
        emit commandRejected(Command::Stop, g.reason);
        return g;
    }
    if (m_stopPhase != StopPhase::Idle)
        return {false, QStringLiteral("停止已在进行中")};

    m_stopPhase = StopPhase::WaitM3Clear;
    m_stopDeadlineMs = m_nowMs() + kStartStopTimeoutMs;
    m_stopTimeoutArmed = true;
    emit commandAccepted(Command::Stop);
    if (m_transport.startPulse && m_transport.startPulse(kM102)) {
        emit commandPending(Command::Stop);
    } else {
        finishCommand(Command::Stop, false, QStringLiteral("M102 脉冲发送失败"));
    }
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::estopSet()
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::EstopSet, s);
    if (!g.accepted) {
        emit commandRejected(Command::EstopSet, g.reason);
        return g;
    }
    // M100=1 is idempotent and safe to retry; the UI shows 待确认 until the
    // snapshot shows M0=1 or M100=1 (spec §8.4).
    // The newest command wins: a set supersedes any in-flight release, so the
    // stale release pending state is cleared and its flow converges instead of
    // hanging on 待确认 forever (spec §13: every flow converges to a result).
    m_estopSetPending = true;
    if (m_estopReleasePending) {
        m_estopReleasePending = false;
        emit commandResult(Command::EstopRelease, false,
                           QStringLiteral("已被新的急停置位取代"));
    }
    emit commandAccepted(Command::EstopSet);
    beginWrite(Command::EstopSet, kM100, true, CommandPriority::Safety);
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::estopRelease()
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::EstopRelease, s);
    if (!g.accepted) {
        emit commandRejected(Command::EstopRelease, g.reason);
        return g;
    }
    m_estopReleasePending = true;
    if (m_estopSetPending) {
        m_estopSetPending = false;
        emit commandResult(Command::EstopSet, false,
                           QStringLiteral("已被新的急停解除取代"));
    }
    emit commandAccepted(Command::EstopRelease);
    beginWrite(Command::EstopRelease, kM100, false, CommandPriority::Safety);
    return {true, QString()};
}

ControlCoordinator::CommandResult ControlCoordinator::manualHold(quint16 address, bool pressed)
{
    // Hold commands are only M106/M107/M108 (spec §10.7); reject others.
    if (address != kM106 && address != kM107 && address != kM108) {
        emit commandRejected(Command::ManualCommand, QStringLiteral("不支持的手动命令地址"));
        return {false, QStringLiteral("不支持的手动命令地址")};
    }
    // Release (write 0) must bypass machine-state interlocks: if a fault/estop
    // latches while the button is held, the release must still be sent so the
    // continuous command clears immediately (spec §10.7 松开写 0, §13 立即请求
    // 清零). It must NOT bypass the permission check: 手动命令仅管理员 (spec
    // §10.7, §11.4 所有写命令统一校验).
    if (!pressed) {
        const PermissionResult p = permission(Command::ManualCommand);
        if (!p.allowed) {
            emit commandRejected(Command::ManualCommand, p.reason);
            return {false, p.reason};
        }
        if (m_transport.writeHold && m_transport.writeHold(address, false)) {
            emit commandAccepted(Command::ManualCommand);
            emit commandResult(Command::ManualCommand, true, QString());
            return {true, QString()};
        }
        emit commandRejected(Command::ManualCommand, QStringLiteral("命令发送失败"));
        return {false, QStringLiteral("命令发送失败")};
    }
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::ManualCommand, s);
    if (!g.accepted) {
        emit commandRejected(Command::ManualCommand, g.reason);
        return g;
    }
    if (m_transport.writeHold && m_transport.writeHold(address, true)) {
        emit commandAccepted(Command::ManualCommand);
        emit commandResult(Command::ManualCommand, true, QString());
        return {true, QString()};
    }
    emit commandRejected(Command::ManualCommand, QStringLiteral("命令发送失败"));
    return {false, QStringLiteral("命令发送失败")};
}

ControlCoordinator::CommandResult ControlCoordinator::manualLatch(quint16 address, bool value)
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::ManualCommand, s);
    if (!g.accepted) {
        emit commandRejected(Command::ManualCommand, g.reason);
        return g;
    }
    // The latched manual command is M109 (stop gate) only (spec §10.7).
    if (address != kM109) {
        emit commandRejected(Command::ManualCommand, QStringLiteral("不支持的手动命令地址"));
        return {false, QStringLiteral("不支持的手动命令地址")};
    }
    if (m_transport.writeCoil && m_transport.writeCoil(address, value, CommandPriority::Normal)) {
        emit commandAccepted(Command::ManualCommand);
        emit commandResult(Command::ManualCommand, true, QString());
        return {true, QString()};
    }
    emit commandRejected(Command::ManualCommand, QStringLiteral("命令发送失败"));
    return {false, QStringLiteral("命令发送失败")};
}

ControlCoordinator::CommandResult ControlCoordinator::bypass(quint16 address, bool value)
{
    const DeviceSnapshot s = m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData()));
    CommandResult g = gate(Command::Bypass, s);
    if (!g.accepted) {
        emit commandRejected(Command::Bypass, g.reason);
        return g;
    }
    // 屏蔽 addresses are M105 (passthrough) and M42/M110/M111 (spec §10.8).
    if (address != kM42 && address != kM105 && address != kM110 && address != kM111) {
        emit commandRejected(Command::Bypass, QStringLiteral("不支持的屏蔽地址"));
        return {false, QStringLiteral("不支持的屏蔽地址")};
    }
    if (m_transport.writeCoil && m_transport.writeCoil(address, value, CommandPriority::Normal)) {
        emit commandAccepted(Command::Bypass);
        emit commandResult(Command::Bypass, true, QString());
        return {true, QString()};
    }
    emit commandRejected(Command::Bypass, QStringLiteral("命令发送失败"));
    return {false, QStringLiteral("命令发送失败")};
}

void ControlCoordinator::logoutClear()
{
    // 注销/会话超时: try to clear M42/M106-M111 (spec §11.5, §13). M100 is
    // never touched; M105 模式选择保持不变 (spec §10.8).
    if (m_transport.writeCoil) {
        m_transport.writeCoil(kM42, false, CommandPriority::Normal);
        for (quint16 a = kM106; a <= kM111; ++a)
            m_transport.writeCoil(a, false, CommandPriority::Normal);
    }
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
    if (m_estopReleasePending && (!s.m0() || !s.m100())) {
        m_estopReleasePending = false;
        emit commandResult(Command::EstopRelease, true, QStringLiteral("急停已解除"));
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

void ControlCoordinator::onWriteCompleted(quint16 address, bool ok)
{
    if (!m_pendingWriteCmd.has_value() || m_pendingWriteAddr != address)
        return;
    const Command cmd = *m_pendingWriteCmd;
    m_pendingWriteCmd.reset();

    switch (cmd) {
    case Command::Reset:
        // A late async write failure only matters while the reset flow is
        // still waiting on that write; otherwise the flow has already
        // converged (spec §13: never double-report after convergence).
        if (!ok && m_resetPhase != ResetPhase::Idle) {
            finishCommand(Command::Reset, false, QStringLiteral("写 M104 失败"));
        }
        // ok: stay in WaitManual until the snapshot shows M1=1.
        break;
    case Command::AdjustWidth:
        if (!ok && m_adjustPhase != AdjustPhase::Idle) {
            finishCommand(Command::AdjustWidth, false, QStringLiteral("写 D128 失败"));
            break;
        }
        if (m_adjustPhase == AdjustPhase::WaitTargetWrite) {
            // D128 confirmed: send the M43 pulse (spec §10.3 step 4).
            if (m_transport.startPulse && m_transport.startPulse(kM43)) {
                m_adjustPhase = AdjustPhase::WaitResult;
                // hmi_timeout = plc_timeout + 3 (spec §10.3).
                const qint32 diff = qAbs(qint32(m_adjustTarget.value_or(0))
                                         - qint32(m_adjustStartWidth.value_or(0)));
                const qint32 speed = qMax<qint32>(1, m_adjustSpeed.value_or(1));
                qint32 plc = (diff + speed - 1) / speed + 5;
                plc = qBound<qint32>(10, plc, 360);
                m_adjustDeadlineMs = m_nowMs() + (plc + 3) * 1000;
                m_adjustTimeoutArmed = true;
                emit commandPending(Command::AdjustWidth);
            } else {
                finishCommand(Command::AdjustWidth, false,
                              QStringLiteral("M43 脉冲发送失败"));
            }
        }
        break;
    case Command::EstopSet:
        if (!ok && m_estopSetPending) {
            m_estopSetPending = false;
            finishCommand(Command::EstopSet, false, QStringLiteral("写 M100 失败"));
        }
        // ok: the snapshot still decides (spec §8.4).
        break;
    case Command::EstopRelease:
        if (!ok && m_estopReleasePending) {
            m_estopReleasePending = false;
            finishCommand(Command::EstopRelease, false, QStringLiteral("写 M100 失败"));
        }
        break;
    case Command::ModeSwitch:
        if (!ok && m_modePending) {
            // A failed M104 write surfaces as a write failure, not 模式切换超时.
            m_modePending = false;
            emit commandResult(Command::ModeSwitch, false, QStringLiteral("写 M104 失败"));
        }
        // ok: stay pending until the snapshot shows M1/M2 (spec §11.2).
        break;
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
        m_adjustStartWidth.reset();
        m_adjustSpeed.reset();
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
    m_pendingWriteCmd.reset();
    if (m_modePending) {
        m_modePending = false;
        emit commandResult(Command::ModeSwitch, false, QStringLiteral("通讯中断"));
    }
    // Estop set/release stay pending: M100 is idempotent and confirmed by the
    // next snapshot after reconnect (spec §8.4, §10.6).
}

// --- flow snapshot handlers -------------------------------------------------

void ControlCoordinator::onResetSnapshot(const DeviceSnapshot &s)
{
    if (m_resetPhase == ResetPhase::WaitManual) {
        if (s.m1()) {
            if (m_transport.startPulse && m_transport.startPulse(kM103)) {
                m_resetPhase = ResetPhase::Homing;
                emit commandPending(Command::Reset);
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

void ControlCoordinator::beginWrite(Command cmd, quint16 address, bool value,
                                    CommandPriority priority)
{
    m_pendingWriteCmd = cmd;
    m_pendingWriteAddr = address;
    if (!m_transport.writeCoil || !m_transport.writeCoil(address, value, priority)) {
        m_pendingWriteCmd.reset();
        finishCommand(cmd, false, QStringLiteral("命令发送失败"));
    }
}

void ControlCoordinator::beginWriteReg(Command cmd, quint16 address, quint16 value,
                                       CommandPriority priority)
{
    m_pendingWriteCmd = cmd;
    m_pendingWriteAddr = address;
    if (!m_transport.writeRegister || !m_transport.writeRegister(address, value, priority)) {
        m_pendingWriteCmd.reset();
        finishCommand(cmd, false, QStringLiteral("命令发送失败"));
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
        m_adjustStartWidth.reset();
        m_adjustSpeed.reset();
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
    emit commandResult(cmd, ok, detail);
}

void ControlCoordinator::setResetTimeoutSec(int sec)
{
    m_cfg.resetTimeoutSec = qBound(30, sec, 600); // spec §10.2: 30-600
}

} // namespace hlm
