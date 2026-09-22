#pragma once

#include <QWidget>
#include <QHash>

#include "ui/pages/manual_control_model.h"

class QLabel;
class QHideEvent;
class QSpinBox;

namespace hlm {

class HoldButton;
class PermissionButton;
class ValueDisplay;
class ShellModel;

// 手动控制 page (spec §11.3): 皮带点动、调宽正反转、挡停、D220、M42、M105、
// M110、M111; 机器命令操作员只读 (spec §11.4), 调宽速度 D220 由管理员在此页
// 直接配置 (user decision 2026-09-22).
//
// The page binds ManualControlModel to widgets. It never touches Modbus or
// SQL: manualHoldRequested / manualLatchRequested / bypassRequested intents
// are emitted as signals for the app shell (Task 20) to wire to the
// ControlCoordinator. All state shown comes from snapshot readback bits
// (spec §11.2: 命令不得乐观更新状态).
//
// HoldButtons (M106/M107/M108) are registered with MainWindow via
// registerHoldWidget() so page switch / modal dialog / logout / window
// deactivation cancel the active hold (spec §10.7). The page forwards
// holdChanged to manualHoldRequested.
//
// 安全屏蔽 (M110/M111) is two-step: the first click arms a "确认屏蔽?" state,
// the second dispatches (spec §10.8 二次确认). While a shield is active
// (readback m110()/m111()=1) the page shows a persistent amber banner.
class ManualControlPage : public QWidget
{
    Q_OBJECT

public:
    explicit ManualControlPage(ShellModel &model, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    HoldButton *jogButton() const { return m_jog; }
    HoldButton *widthFwdButton() const { return m_widthFwd; }
    HoldButton *widthRevButton() const { return m_widthRev; }
    PermissionButton *stopGateButton() const { return m_stopGate; }
    PermissionButton *passthroughButton() const { return m_passthrough; }
    PermissionButton *beltContinuousButton() const { return m_beltContinuous; }
    PermissionButton *curtainShieldButton() const { return m_curtainShield; }
    PermissionButton *doorShieldButton() const { return m_doorShield; }
    // 测试信号 M114-M117 (user decision 2026-09-22), keyed by protocol address.
    PermissionButton *simSignalButton(quint16 address) const
    {
        return m_simSignalButtons.value(address, nullptr);
    }
    ValueDisplay *fieldDisplay(const QString &key) const;
    // 调宽速度 D220 editor (user decision 2026-09-22): the spin box is the
    // operator's value, the button dispatches it, the label carries the
    // page-local write result (never silent).
    QSpinBox *widthSpeedSpin() const { return m_widthSpeedSpin; }
    PermissionButton *writeWidthSpeedButton() const { return m_writeWidthSpeed; }
    QString widthSpeedResultText() const;
    QLabel *shieldBanner() const { return m_shieldBanner; }
    QString shieldBannerText() const;
    QLabel *statusLabel() const { return m_statusLabel; }
    QLabel *widthReasonLabel() const { return m_widthReason; }
    QString statusText() const;

public slots:
    // Re-renders every widget from the model's current state.
    void refresh();
    // Page-local result of a 调宽速度 D220 write (user decision 2026-09-22):
    // shown inline next to the editor, so a parameter write is never silent
    // (spec §11.2; the users page reports its own writes the same way).
    void setWidthSpeedWriteResult(bool ok, const QString &detail);

signals:
    // Write intents for the app shell (Task 20). Never emitted optimistically.
    void manualHoldRequested(quint16 address, bool pressed);
    void manualLatchRequested(quint16 address, bool value);
    void bypassRequested(quint16 address, bool value);
    // 测试信号 M114-M117: one 100 ms pulse per simulated station signal
    // (user decision 2026-09-22). Never emitted optimistically.
    void simStationPulseRequested(quint16 address);
    // 调宽速度 D220 write request (user decision 2026-09-22): the app shell
    // validates and submits it, then reports back through
    // setWidthSpeedWriteResult.
    void widthSpeedWriteRequested(quint16 value);

protected:
    // Page switch (QStackedWidget hides the page) clears the armed shield
    // confirmation (spec §10.8 二次确认, §11.1-§11.2 页面切换清零意图).
    void hideEvent(QHideEvent *event) override;

private:
    void buildLayout();
    QWidget *addField(const QString &key, const QString &title);
    void onStopGateClicked();
    void onWriteWidthSpeedClicked();
    void onShieldClicked(PermissionButton *button, quint16 address);
    void disarmShield(PermissionButton *button, quint16 address);

    ShellModel &m_model;
    ManualControlModel m_pageModel;

    HoldButton *m_jog = nullptr;        // M108 皮带点动
    HoldButton *m_widthFwd = nullptr;   // M106 调宽正转
    HoldButton *m_widthRev = nullptr;   // M107 调宽反转
    PermissionButton *m_stopGate = nullptr; // M109 挡停
    // Visible inline reason for the disabled width jogs (M106/M107). PLC-HMI-011
    // D5: the width jogs are the only manual controls that still require homing
    // completion, so their remaining reason must be readable on the page and not
    // only in a tooltip (spec §11.4; the OB-7 surface requirement). Each width
    // button owns its own label so the reason stays adjacent to its control.
    QLabel *m_widthReason = nullptr;
    QLabel *m_widthRevReason = nullptr;
    PermissionButton *m_passthrough = nullptr;   // M105 直通
    PermissionButton *m_beltContinuous = nullptr; // M42 皮带常转
    PermissionButton *m_curtainShield = nullptr; // M110 光栅屏蔽
    PermissionButton *m_doorShield = nullptr;
    QHash<quint16, PermissionButton *> m_simSignalButtons;    // M111 门磁屏蔽
    QLabel *m_shieldBanner = nullptr;
    QLabel *m_statusLabel = nullptr;
    QHash<QString, ValueDisplay *> m_displays;
    // 调宽速度 D220 editor (user decision 2026-09-22): configurable from the
    // manual page. The value shown is the confirmed readback; the write is
    // admin-only and reports its result in m_widthSpeedResult.
    QSpinBox *m_widthSpeedSpin = nullptr;
    PermissionButton *m_writeWidthSpeed = nullptr;
    QLabel *m_widthSpeedResult = nullptr;
    // Last value seeded into the spin box from the readback, so refresh() does
    // not overwrite an edit the operator is typing.
    int m_lastSeededWidthSpeed = 0;
};

} // namespace hlm
