#pragma once

#include <QObject>
#include <QString>

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
//  - Pulses (M101/M102/M103/M43) are submitted through a small transport
//    abstraction (PulseTransport) so the coordinator stays testable against a
//    fake or the SimulatedPlcGateway; the real worker thread wires the
//    PulseStateMachine callbacks into the gateway (spec §7.2, §8.5).
//  - Command lifecycle is tracked per flow: reset waits for M61, adjust waits
//    for M44/M45 with the saved target, start waits for M3, stop waits for
//    M3=0. Timeouts converge to the actual PLC state (spec §13).
//  - M100 is never auto-cleared (spec §10.6, §11.5, §13).
class ControlCoordinator : public QObject
{
    Q_OBJECT

public:
    // Pulse transport abstraction (spec §8.5). The real worker thread routes
    // these into the PulseStateMachine; tests use a recording fake.
    struct PulseTransport {
        // Returns false when the pulse could not be started (offline / a
        // pulse on the same address is already active).
        std::function<bool(quint16 address)> startPulse;
        // Continuous (hold) command: write 1 on press, 0 on release.
        std::function<bool(quint16 address, bool value)> writeHold;
        // Plain write (coil/register) with a priority hint.
        std::function<bool(quint16 address, bool value, CommandPriority priority)> writeCoil;
        std::function<bool(quint16 address, quint16 value, CommandPriority priority)> writeRegister;
    };

    // Config (spec §10.2: HMI defensive reset timeout, admin-configurable).
    struct Config {
        Config() : resetTimeoutSec(120) {}
        explicit Config(int resetTimeout)
            : resetTimeoutSec(qBound(30, resetTimeout, 600)) // clamp 30-600 (spec §10.2)
        {
        }
        int resetTimeoutSec; // 30-600 (spec §10.2)
    };

    // Result of a command submission (accepted vs rejected with a reason).
    struct CommandResult {
        bool accepted = false;
        QString reason;
    };

    // `nowMs` is injected for deterministic timeout tests (same pattern as
    // PulseStateMachine / WatchdogTimer). Defaults to wall clock.
    explicit ControlCoordinator(PulseTransport transport,
                                Config config = Config(),
                                std::function<qint64()> nowMs = nullptr,
                                QObject *parent = nullptr);

    // --- session ------------------------------------------------------------
    void setRole(Role role);
    Role role() const { return m_role; }

    // --- commands (all return a structured result; effects arrive via signals)
    PermissionResult permission(Command cmd) const;
    InterlockResult interlock(Command cmd, const DeviceSnapshot &s,
                              quint16 targetWidth = 0) const;

    // 复位 (spec §10.2). Admin only.
    CommandResult reset();
    // 配方调宽 (spec §10.3). Admin only. `targetWidth` 50-400.
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
    // 注销/会话超时: try to clear M42/M106-M111; M100 is never touched
    // (spec §11.5, §13).
    void logoutClear();

    // --- snapshot / write result feed (from the gateway) ---------------------
    void onSnapshot(const DeviceSnapshot &s);
    void onWriteCompleted(quint16 address, bool ok);
    void onConnectionChanged(bool online);

    // --- state --------------------------------------------------------------
    bool online() const { return m_online; }
    bool hasSnapshot() const { return m_snapshot.has_value(); }
    DeviceSnapshot snapshot() const { return m_snapshot.value_or(DeviceSnapshot(DeviceSnapshotData())); }
    bool resetInProgress() const { return m_resetPhase != ResetPhase::Idle; }
    bool adjustInProgress() const { return m_adjustPhase != AdjustPhase::Idle; }
    bool startInProgress() const { return m_startPhase != StartPhase::Idle; }
    bool stopInProgress() const { return m_stopPhase != StopPhase::Idle; }
    // Saved command context (spec §10.3 step 4).
    std::optional<quint16> adjustTarget() const { return m_adjustTarget; }
    std::optional<quint16> adjustStartWidth() const { return m_adjustStartWidth; }
    std::optional<quint16> adjustSpeed() const { return m_adjustSpeed; }

    // --- config -------------------------------------------------------------
    void setResetTimeoutSec(int sec); // 30-600 (spec §10.2)
    int resetTimeoutSec() const { return m_cfg.resetTimeoutSec; }

signals:
    // Command accepted and dispatched (waiting for PLC confirmation).
    void commandAccepted(Command cmd);
    // Command rejected by permission or interlock (reason for UI display).
    void commandRejected(Command cmd, const QString &reason);
    // Command result confirmed by the PLC snapshot (never optimistic).
    void commandResult(Command cmd, bool ok, const QString &detail);
    // A command is waiting for PLC confirmation (UI shows 发送中/等待确认).
    void commandPending(Command cmd);
    // Continuous-command clear requested (logout / session timeout).
    void continuousCleared();

private:
    enum class ResetPhase { Idle, WaitManual, Homing };
    enum class AdjustPhase { Idle, WaitTargetWrite, WaitResult };
    enum class StartPhase { Idle, WaitM3 };
    enum class StopPhase { Idle, WaitM3Clear };

    CommandResult gate(Command cmd, const DeviceSnapshot &s, quint16 targetWidth = 0);
    void beginWrite(Command cmd, quint16 address, bool value, CommandPriority priority);
    void beginWriteReg(Command cmd, quint16 address, quint16 value, CommandPriority priority);
    void finishCommand(Command cmd, bool ok, const QString &detail);

    void onResetSnapshot(const DeviceSnapshot &s);
    void onAdjustSnapshot(const DeviceSnapshot &s);
    void onStartSnapshot(const DeviceSnapshot &s);
    void onStopSnapshot(const DeviceSnapshot &s);

    PulseTransport m_transport;
    Config m_cfg;
    std::function<qint64()> m_nowMs;
    Role m_role = Role::Anonymous;
    bool m_online = false;
    std::optional<DeviceSnapshot> m_snapshot;

    // Command lifecycle state.
    ResetPhase m_resetPhase = ResetPhase::Idle;
    AdjustPhase m_adjustPhase = AdjustPhase::Idle;
    StartPhase m_startPhase = StartPhase::Idle;
    StopPhase m_stopPhase = StopPhase::Idle;
    std::optional<quint16> m_adjustTarget;
    std::optional<quint16> m_adjustStartWidth;
    std::optional<quint16> m_adjustSpeed;
    qint64 m_resetDeadlineMs = 0; // clock time of the reset timeout
    qint64 m_adjustDeadlineMs = 0;
    qint64 m_startDeadlineMs = 0;
    qint64 m_stopDeadlineMs = 0;
    bool m_resetTimeoutArmed = false;
    // True once the Homing phase has observed M50=1 (home return actually
    // started). Success is only accepted after this, so a lost M103 pulse can
    // never report a false "回原点完成" (spec §10.2 step 3, §13).
    bool m_resetHomingStarted = false;
    bool m_adjustTimeoutArmed = false;
    bool m_startTimeoutArmed = false;
    bool m_stopTimeoutArmed = false;
    // Pending write routing: which command is waiting on a writeCompleted.
    std::optional<Command> m_pendingWriteCmd;
    quint16 m_pendingWriteAddr = 0;
    // Mode switch (waits for M1/M2, spec §11.2).
    bool m_modePending = false;
    std::optional<bool> m_modeTarget;
    qint64 m_modeDeadlineMs = 0;
    // Estop confirmation (spec §8.4: until M0=1 or M100=1 readback).
    bool m_estopSetPending = false;
    bool m_estopReleasePending = false;
};

} // namespace hlm
