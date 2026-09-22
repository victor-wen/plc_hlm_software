#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

namespace hlm {

class ShellModel;

// 手动控制 page model (spec §10.7-§10.8, §11.3, §11.4). Maps the latest
// ShellModel snapshot to the page's gating and readback state. No I/O, no
// commands: the page emits manualHoldRequested / manualLatchRequested /
// bypassRequested intents; this model only computes permission/interlock
// gating and derives all state from CONFIRMED snapshots (spec §11.2: 命令
// 不得乐观更新状态).
//
// - M106/M107/M108: hold commands (press=1, release=0), gated by
//   checkManualCommand (手动模式 M1、回原点 M9、M3=0、无急停、无锁存故障).
// - M109: latched stop gate; state shown from m109() readback.
// - M42/M105/M110/M111: bypass commands, gated by checkBypass (online).
// - M110/M111 安全屏蔽: two-step confirm (armShield then dispatch); the armed
//   state resets on page switch (page hideEvent) and on gate change (e.g.
//   going offline), so a stale confirmation can never dispatch (spec §10.8).
class ManualControlModel : public QObject
{
    Q_OBJECT

public:
    explicit ManualControlModel(const ShellModel &model, QObject *parent = nullptr);

    // --- gating (permission + interlock, spec §11.4) -------------------------
    // canManual()/manualUnmetReasons() are the address-less verdict: they keep
    // the strict four-command gate (including M61=1 回原点完成 and M50=0) for
    // callers that have no per-command context. The page uses the per-command
    // accessors below, because PLC-HMI-011 D5 splits the manual interlock:
    // M106/M107 (调宽) require homing completion, M108 (皮带点动) and M109
    // (挡停) do not.
    bool canManual() const;
    QStringList manualUnmetReasons() const;
    bool canWidthJog() const;
    QStringList widthJogUnmetReasons() const;
    bool canBeltJog() const;
    QStringList beltJogUnmetReasons() const;
    bool canStopGate() const;
    QStringList stopGateUnmetReasons() const;
    bool canBypass() const;
    QStringList bypassUnmetReasons() const;
    // 测试信号 M15/M114-M117 (user decision 2026-09-22): 仅管理员 + 在线, 无机器
    // 状态前置条件, 以便在流程运行中注入模拟信号.
    bool canSimSignal() const;
    QStringList simSignalUnmetReasons() const;

    // --- shield two-step confirm (spec §10.8) ---------------------------------
    // Arms the confirmation for M110/M111 with the target derived from the
    // current readback (true when inactive, false when active).
    void armShield(quint16 address);
    void disarmShield(quint16 address);
    void disarmAllShields();
    bool shieldArmed(quint16 address) const;
    // Target of the armed confirmation (true = 屏蔽, false = 恢复).
    std::optional<bool> shieldTarget(quint16 address) const;

    // --- snapshot readback (spec §11.2: 状态来自回读位, 不是按钮状态) ---------
    bool m42() const;
    bool m105() const;
    bool m109() const;
    bool m110() const;
    bool m111() const;
    bool m106() const;
    bool m107() const;
    bool m108() const;
    // 安全屏蔽生效 (M110 或 M111): 顶部持续显示琥珀色横幅 (spec §10.8).
    bool shieldActive() const;

    // --- D220 调宽速度 (readback + admin write, user decision 2026-09-22) -------
    bool widthSpeedValid() const;
    quint16 widthSpeed() const;
    // 调宽速度可配置: 仅管理员 + 在线 + 回读有效. The page renders the editor
    // from this verdict and shows every unmet reason inline (spec §11.4).
    bool canWriteWidthSpeed() const;
    QStringList widthSpeedWriteUnmetReasons() const;

    // --- D130 当前宽度 实时回读 (0.1mm raw, see domain/width_units.h) ---------
    bool currentWidthValid() const;
    quint16 currentWidth() const;

    // --- status ---------------------------------------------------------------
    QString statusText() const;

private slots:
    // Resets armed shield confirmations when the bypass gate closes (e.g.
    // going offline), so a stale confirmation can never dispatch (spec §10.8).
    void onShellStateChanged();

private:
    const ShellModel &m_model;
    std::optional<quint16> m_shieldArmed; // M110/M111 address
    std::optional<bool> m_shieldTarget;
};

} // namespace hlm
