#pragma once

#include <QWidget>
#include <QHash>

#include "ui/pages/manual_control_model.h"

class QLabel;
class QHideEvent;

namespace hlm {

class HoldButton;
class PermissionButton;
class ValueDisplay;
class ShellModel;

// 手动控制 page (spec §11.3): 皮带点动、调宽正反转、挡停、D220、M42、M105、
// M110、M111; 操作员只读 (spec §11.4).
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
    ValueDisplay *fieldDisplay(const QString &key) const;
    QLabel *shieldBanner() const { return m_shieldBanner; }
    QString shieldBannerText() const;
    QLabel *statusLabel() const { return m_statusLabel; }
    QString statusText() const;

public slots:
    // Re-renders every widget from the model's current state.
    void refresh();

signals:
    // Write intents for the app shell (Task 20). Never emitted optimistically.
    void manualHoldRequested(quint16 address, bool pressed);
    void manualLatchRequested(quint16 address, bool value);
    void bypassRequested(quint16 address, bool value);

protected:
    // Page switch (QStackedWidget hides the page) clears the armed shield
    // confirmation (spec §10.8 二次确认, §11.1-§11.2 页面切换清零意图).
    void hideEvent(QHideEvent *event) override;

private:
    void buildLayout();
    ValueDisplay *addField(const QString &key, const QString &title);
    void onStopGateClicked();
    void onShieldClicked(PermissionButton *button, quint16 address);
    void disarmShield(PermissionButton *button, quint16 address);

    ShellModel &m_model;
    ManualControlModel m_pageModel;

    HoldButton *m_jog = nullptr;        // M108 皮带点动
    HoldButton *m_widthFwd = nullptr;   // M106 调宽正转
    HoldButton *m_widthRev = nullptr;   // M107 调宽反转
    PermissionButton *m_stopGate = nullptr; // M109 挡停
    PermissionButton *m_passthrough = nullptr;   // M105 直通
    PermissionButton *m_beltContinuous = nullptr; // M42 皮带常转
    PermissionButton *m_curtainShield = nullptr; // M110 光栅屏蔽
    PermissionButton *m_doorShield = nullptr;    // M111 门磁屏蔽
    QLabel *m_shieldBanner = nullptr;
    QLabel *m_statusLabel = nullptr;
    QHash<QString, ValueDisplay *> m_displays;
};

} // namespace hlm
