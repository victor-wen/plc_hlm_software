#pragma once

#include <QWidget>
#include <QVector>

#include "application/permission_policy.h"

class QVBoxLayout;

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

public slots:
    // Recomputes permission/interlock reasons from the model.
    void refresh();

signals:
    // Emitted for mode switch (manual=false, auto=true), start, stop, reset,
    // estop set/release. The owner routes to the coordinator.
    void actionRequested(Command cmd);
    void loginLogoutRequested();

private:
    ShellModel &m_model;
    PermissionButton *m_manual = nullptr;
    PermissionButton *m_auto = nullptr;
    PermissionButton *m_start = nullptr;
    PermissionButton *m_stop = nullptr;
    PermissionButton *m_reset = nullptr;
    PermissionButton *m_login = nullptr;
    PermissionButton *m_estop = nullptr; // separated danger button
};

} // namespace hlm