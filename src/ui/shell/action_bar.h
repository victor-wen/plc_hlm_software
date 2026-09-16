#pragma once

#include <QWidget>
#include <QVector>

#include "application/permission_policy.h"

class QVBoxLayout;
class QLabel;
class QScrollArea;

namespace hlm {

class PermissionButton;
class ShellModel;

// Right fixed action bar, ~192 px (spec §11.1): 手动、自动、启动、停止、复位、
// 登录/注销 and the SEPARATE software estop (spec §10.6, fixed red danger
// style, visually isolated from normal actions).
//
// The bar computes enable/disable + reasons from ShellModel state via
// PermissionPolicy + InterlockRules (spec §11.4). It does NOT execute
// commands; it emits actionRequested(Command) and the owner (MainWindow /
// app wiring) routes them to the ControlCoordinator.
class ActionBar : public QWidget
{
    Q_OBJECT

public:
    explicit ActionBar(ShellModel &model, QWidget *parent = nullptr);

    PermissionButton *estopButton() const { return m_estop; }
    PermissionButton *startButton() const { return m_start; }
    PermissionButton *stopButton() const { return m_stop; }
    PermissionButton *resetButton() const { return m_reset; }
    PermissionButton *manualButton() const { return m_manual; }
    PermissionButton *autoButton() const { return m_auto; }
    PermissionButton *loginButton() const { return m_login; }

    // Persistent, non-modal machine-command status text (state + detail) for
    // the latest projected OperatorCommandStatus (D8, contract presentation
    // "machine_commands: persistent non-modal shell status").
    QLabel *commandStatusLabel() const { return m_commandStatus; }

    // Responsive layout seams (PLC-HMI-006 D1/D2): the scrollable action group
    // and the pinned safety strip (Stop + software estop) that never scroll.
    QScrollArea *actionsScrollArea() const { return m_scroll; }
    QWidget *safetyStrip() const { return m_safetyStrip; }

public slots:
    // Recomputes permission/interlock reasons from the model.
    void refresh();

signals:
    // Emitted for start, stop, reset, estop set/release. The owner routes to
    // the coordinator.
    void actionRequested(Command cmd);
    // Emitted for mode switch with the target direction: manual=false,
    // auto=true. The owner routes to ControlCoordinator::setMode(bool).
    void modeSwitchRequested(bool autoMode);
    void loginLogoutRequested();

private:
    // Renders the latest OperatorCommandStatus into the status label.
    void refreshCommandStatus();

    ShellModel &m_model;
    QScrollArea *m_scroll = nullptr;      // scrollable non-safety actions
    QWidget *m_safetyStrip = nullptr;     // pinned Stop + estop
    PermissionButton *m_manual = nullptr;
    PermissionButton *m_auto = nullptr;
    PermissionButton *m_start = nullptr;
    PermissionButton *m_stop = nullptr;
    PermissionButton *m_reset = nullptr;
    PermissionButton *m_login = nullptr;
    PermissionButton *m_estop = nullptr; // separated danger button
    QLabel *m_commandStatus = nullptr;   // persistent command status text
};

} // namespace hlm
