#include "app/lifecycle_controller.h"

#include <QCoreApplication>
#include <QEvent>
#include <QTimer>

#include "application/control_coordinator.h"
#include "ui/MainWindow.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

namespace hlm {

LifecycleController::LifecycleController(
    ShellModel *shell, ControlCoordinator *coordinator, MainWindow *window,
    UsersSettingsPage *usersPage,
    std::function<void(const QString &, const QString &, const QString &,
                       AuditResult, const QString &)> auditFn,
    std::function<void()> stopHeartbeatFn, QObject *parent)
    : QObject(parent)
    , m_shell(shell)
    , m_coordinator(coordinator)
    , m_window(window)
    , m_usersPage(usersPage)
    , m_auditFn(std::move(auditFn))
    , m_stopHeartbeatFn(std::move(stopHeartbeatFn))
{
    m_sessionTimer = new QTimer(this);
    m_sessionTimer->setInterval(1000);
    connect(m_sessionTimer, &QTimer::timeout, this,
            &LifecycleController::onSessionTick);
    if (QCoreApplication::instance())
        QCoreApplication::instance()->installEventFilter(this);
}

void LifecycleController::startSessionTimer()
{
    m_remainingSec = m_sessionTimeoutSec;
    if (m_usersPage)
        m_usersPage->setSessionRemainingSec(m_remainingSec);
    m_sessionTimer->start();
}

void LifecycleController::stopSessionTimer()
{
    m_sessionTimer->stop();
}

void LifecycleController::resetSessionTimer()
{
    m_remainingSec = m_sessionTimeoutSec;
    if (m_usersPage)
        m_usersPage->setSessionRemainingSec(m_remainingSec);
    m_sessionTimer->start();
}

void LifecycleController::setSessionTimeoutSec(int sec)
{
    m_sessionTimeoutSec = qMax(1, sec);
    resetSessionTimer();
}

void LifecycleController::onSessionTick()
{
    if (m_remainingSec <= 0)
        return; // already expired; the page emitted the logout requests once
    // A continuous hold is active operation even when the pointer is stationary.
    if (!m_username.isEmpty() && m_window && m_window->hasActiveHolds()) {
        resetSessionTimer();
        return;
    }
    --m_remainingSec;
    if (m_usersPage)
        m_usersPage->setSessionRemainingSec(m_remainingSec);
    if (m_remainingSec <= 0) {
        // The page emits logoutRequested + logoutClearRequested on expiry
        // (spec §11.5); the timer keeps ticking but stays at 0.
        m_sessionTimer->stop();
    }
}

bool LifecycleController::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    if (!m_username.isEmpty()) {
        switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
        case QEvent::TouchBegin:
        case QEvent::TabletPress:
            resetSessionTimer();
            break;
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void LifecycleController::onLoginSucceeded(const UserRecord &user)
{
    m_username = user.username;
    if (m_shell)
        m_shell->setUser(user.username, user.role);
    if (m_coordinator)
        m_coordinator->setRole(user.role);
    resetSessionTimer();
    if (m_auditFn)
        m_auditFn(QStringLiteral("auth.login"), QStringLiteral("user"),
                  QString(), AuditResult::Success, QString());
}

void LifecycleController::onLogoutRequested()
{
    doLogout();
}

void LifecycleController::onLogoutClearRequested()
{
    // 注销触发 M42、M106-M111 清零流程 (spec §11.5). M100 永不自动清除.
    if (m_coordinator)
        m_coordinator->logoutClear();
    if (m_window)
        m_window->clearHoldIntents();
}

void LifecycleController::doLogout()
{
    if (m_auditFn && !m_username.isEmpty())
        m_auditFn(QStringLiteral("auth.logout"), QStringLiteral("user"),
                  QString(), AuditResult::Success, QString());
    m_username.clear();
    if (m_shell)
        m_shell->clearUser();
    if (m_coordinator)
        m_coordinator->setRole(Role::Anonymous);
    stopSessionTimer();
}

void LifecycleController::enterRestrictedMode(const QString &reason)
{
    m_restricted = true;
    m_restrictedReason = reason;
    // 受限模式: 只保留在线停止和置软件急停 (spec §13). 强制注销所有用户.
    doLogout();
}

bool LifecycleController::commandAllowed(Command cmd) const
{
    if (!m_restricted)
        return true;
    // 受限模式只保留在线停止和置软件急停 (spec §13).
    return cmd == Command::Stop || cmd == Command::EstopSet;
}

void LifecycleController::shutdown()
{
    // 应用正常退出: 清除 M42/M106-M111, 然后停止 M112 并断开串口 (spec §13).
    // M100 在任何退出路径都不得自动清除.
    if (m_coordinator)
        m_coordinator->logoutClear();
    if (m_window)
        m_window->clearHoldIntents();
    if (m_stopHeartbeatFn)
        m_stopHeartbeatFn();
    stopSessionTimer();
}

} // namespace hlm
