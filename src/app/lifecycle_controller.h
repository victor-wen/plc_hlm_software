#pragma once

// Lifecycle and session state (spec §7, §11.5, §13).
//
// Owned by the UI main thread (like every QWidget and the coordinator,
// spec §7.1). Responsibilities:
//  - Session countdown: 15 分钟无操作自动注销, 提前 60 秒提示 (spec §11.5).
//    A QTimer drives one tick per second; the remaining seconds are fed to
//    UsersSettingsPage::setSessionRemainingSec, which emits the logout +
//    M42/M106-M111 clear requests on expiry. The tick is exposed as a public
//    method so integration tests drive the countdown deterministically.
//  - Login state: onLoginSucceeded -> ShellModel::setUser +
//    ControlCoordinator::setRole; logout -> clearUser + setRole(Anonymous).
//  - Restricted mode (spec §13): when the database is unavailable only
//    online-stop and software-estop remain; every other command is blocked
//    at the application layer.
//  - Shutdown (spec §13): clear M42/M106-M111, stop the M112 heartbeat, then
//    stop the gateway and the database. M100 is never auto-cleared.

#include <QObject>
#include <QString>

#include <functional>

#include "application/permission_policy.h"
#include "ports/repositories.h"

class QTimer;

namespace hlm {

class ShellModel;
class ControlCoordinator;
class MainWindow;
class UsersSettingsPage;

class LifecycleController : public QObject
{
    Q_OBJECT

public:
    // `auditFn(action, target, redactedParams, result, reason)` and
    // `stopHeartbeatFn()` are provided by the composition root.
    LifecycleController(ShellModel *shell, ControlCoordinator *coordinator,
                        MainWindow *window, UsersSettingsPage *usersPage,
                        std::function<void(const QString &, const QString &,
                                           const QString &, AuditResult,
                                           const QString &)> auditFn,
                        std::function<void()> stopHeartbeatFn,
                        QObject *parent = nullptr);

    // --- session (spec §11.5) -------------------------------------------------
    void startSessionTimer();
    void stopSessionTimer();
    void resetSessionTimer();
    void setSessionTimeoutSec(int sec);
    int sessionTimeoutSec() const { return m_sessionTimeoutSec; }
    int sessionRemainingSec() const { return m_remainingSec; }
    // One countdown tick (QTimer-driven in production; called directly by
    // deterministic integration tests).
    void onSessionTick();

    // --- login / logout (spec §11.5) ------------------------------------------
    void onLoginSucceeded(const UserRecord &user);
    void onLogoutRequested();
    void onLogoutClearRequested();
    QString currentUsername() const { return m_username; }

    // --- restricted mode (spec §13) --------------------------------------------
    void enterRestrictedMode(const QString &reason);
    bool restricted() const { return m_restricted; }
    QString restrictedReason() const { return m_restrictedReason; }
    // False when restricted mode blocks the command (only Stop/EstopSet stay).
    bool commandAllowed(Command cmd) const;

    // --- shutdown (spec §13) ----------------------------------------------------
    void shutdown();

private:
    void doLogout();

    ShellModel *m_shell;
    ControlCoordinator *m_coordinator;
    MainWindow *m_window;
    UsersSettingsPage *m_usersPage;
    std::function<void(const QString &, const QString &, const QString &,
                       AuditResult, const QString &)>
        m_auditFn;
    std::function<void()> m_stopHeartbeatFn;

    QTimer *m_sessionTimer = nullptr;
    int m_sessionTimeoutSec = 900; // 15 分钟 (spec §11.5)
    int m_remainingSec = 900;
    QString m_username;
    bool m_restricted = false;
    QString m_restrictedReason;
};

} // namespace hlm
