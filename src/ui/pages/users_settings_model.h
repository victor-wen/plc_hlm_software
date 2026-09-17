#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include "domain/serial_connection_settings.h"
#include "ports/repositories.h" // UserRecord, LoginResult, Role

namespace hlm {

class ShellModel;

// 用户与设置 page model (spec §8.1, §11.3-§11.5). Pure mapping/state over
// ShellModel (role/snapshot) plus externally-fed auth/settings data (wired by
// the app shell, Task 20). No I/O, no commands: the page emits request signals
// for login/create-admin/user CRUD/serial save/parameter writes; this model
// only computes permission gating, range validation and display state.
//
// 无乐观状态 (spec §11.2): login results, parameter write results and user
// lists arrive ONLY via setLoginResult / setParameterWriteResult / setUsers;
// parameter values shown come from the snapshot (D122/D204/D220), never from
// the editor.
//
// 敏感字段不渲染 (spec §11.4): when locked (未登录/操作员) the page shows a
// lock panel and renders NO user list, password fields, serial config or
// parameter values.
class UsersSettingsModel : public QObject
{
    Q_OBJECT

public:
    explicit UsersSettingsModel(const ShellModel &model, QObject *parent = nullptr);

    // --- permission / lock panel (spec §11.4) --------------------------------
    // 用户、通讯和参数设置仅管理员. 未登录/操作员 -> locked.
    bool locked() const;
    bool isAdmin() const;

    // --- first-run admin creation (spec §11.5) --------------------------------
    // Fed by the app shell (Task 20 wires DatabaseService::needsInitialAdmin).
    void setNeedsInitialAdmin(bool needs);
    bool needsInitialAdmin() const { return m_needsInitialAdmin; }

    // --- login (spec §11.5) ---------------------------------------------------
    // Result fed from DatabaseService::loginResult (Task 20). Never derived
    // from the UI. Account-specific failure reasons are displayed generically
    // so login responses cannot be used to enumerate usernames.
    void setLoginResult(const LoginResult &result);
    bool loginLocked() const { return m_loginLocked; }
    QString loginStatusText() const;

    // --- session countdown (spec §11.5) ---------------------------------------
    // Fed by the app shell (Task 20 owns the 15 分钟 idle timer). 提前 60 秒
    // 提示; 0 -> expired (page emits logout + clear requests).
    void setSessionRemainingSec(int seconds);
    bool sessionWarningActive() const { return m_sessionWarning; }
    bool sessionExpired() const { return m_sessionExpired; }
    QString sessionStatusText() const;

    // --- users (admin only) ---------------------------------------------------
    void setUsers(const QVector<UserRecord> &users);
    QVector<UserRecord> users() const { return m_users; }

    // --- serial config (admin only, spec §8.1) --------------------------------
    // The canonical value is the transport-neutral domain type
    // (SerialConnectionSettings); the model only renders/validates it and
    // never redefines it (spec ARCH-005).
    SerialConnectionSettings serialConfig() const { return m_serial; }
    void setSerialConfig(const SerialConnectionSettings &cfg);
    bool serialConfigValid() const;
    QStringList serialConfigReasons() const;

    // --- admin parameters (admin only, spec §11.3) ----------------------------
    // Edited values (the displayed values come from the snapshot).
    int editedD122() const { return m_editedD122; }
    int editedD204() const { return m_editedD204; }
    int editedD220() const { return m_editedD220; }
    void setEditedD122(int v);
    void setEditedD204(int v);
    void setEditedD220(int v);

    // D122 皮带速度 100-20000 Hz.
    bool d122Valid() const;
    // D204 脉冲当量 1-32767 (有符号 MUL 范围).
    bool d204Valid() const;
    // D220 调宽速度 1-15.
    bool d220Valid() const;
    // All parameter validation reasons (empty when everything is valid).
    QStringList paramReasons() const;

    // --- parameter write result (spec §11.2: 无乐观状态) ----------------------
    // Marks a write as in-flight (waiting for the confirmed result).
    void setParameterWritePending();
    void setParameterWriteResult(bool ok, const QString &detail);
    bool paramWritePending() const { return m_paramWritePending; }
    QString paramStatusText() const;

signals:
    // Re-render request for the page (model state changed).
    void stateChanged();

private:
    const ShellModel &m_model;
    bool m_needsInitialAdmin = false;
    bool m_loginLocked = false;
    QString m_loginError;
    bool m_sessionWarning = false;
    bool m_sessionExpired = false;
    int m_sessionRemainingSec = 900; // 15 分钟默认 (spec §11.5)
    QVector<UserRecord> m_users;
    SerialConnectionSettings m_serial;
    int m_editedD122 = 5000; // D122 权威默认 5000 Hz
    int m_editedD204 = 128;  // D204 权威默认 128 脉冲/mm
    int m_editedD220 = 15;   // D220 权威默认 15 mm/s
    bool m_paramWritePending = false;
    QString m_paramStatus;
};

} // namespace hlm
