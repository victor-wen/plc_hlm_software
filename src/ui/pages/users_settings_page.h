#pragma once

#include <QWidget>
#include <QHash>
#include <QPointer>
#include <QVector>

#include "ui/pages/users_settings_model.h"

class QLabel;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QListWidget;
class QStackedWidget;
class QPushButton;

namespace hlm {

class ShellModel;
class ValueDisplay;
class LoginDialog;
class AdminPasswordDialog;

// 用户与设置 page (spec §8.1, §11.3-§11.5): 登录/首次管理员创建、用户增删改密、
// 串口配置、会话倒计时提示、D122/D204/D220 管理员参数.
//
// The page binds UsersSettingsModel to widgets. It never touches Modbus or SQL:
// every operation is emitted as a request signal for the app shell (Task 20) to
// wire to DatabaseService / ControlCoordinator. Results arrive ONLY via the
// feed slots (setLoginResult / setUsers / setParameterWriteResult / snapshot);
// the page never shows optimistic success (spec §11.2).
//
// 敏感字段不渲染 (spec §11.4): 未登录/操作员看到锁定面板 (需要管理员登录),
// 用户列表、密码、通讯配置和参数值一律不渲染. 首次启动 (无用户) 显示强制创建
// 管理员面板, 无默认密码 (spec §11.5).
//
// D204 脉冲当量单独显示"非专业人员勿修改", 修改前要求再次输入管理员密码
// (AdminPasswordDialog 二次验证, spec §11.3).
class UsersSettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit UsersSettingsPage(ShellModel &model, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    QWidget *lockedPanel() const { return m_lockedPanel; }
    QWidget *createAdminPanel() const { return m_createAdminPanel; }
    QWidget *adminPanel() const { return m_adminPanel; }
    // The currently active panel (locked / create-admin / admin).
    QWidget *currentPanel() const;

    QListWidget *userList() const { return m_userList; }
    QLineEdit *comPortEdit() const { return m_comPort; }
    QSpinBox *stationSpin() const { return m_station; }
    QComboBox *baudRateCombo() const { return m_baudRate; }
    QComboBox *stopBitsCombo() const { return m_stopBits; }
    QComboBox *parityCombo() const { return m_parity; }
    QSpinBox *timeoutSpin() const { return m_timeout; }
    QSpinBox *readRetriesSpin() const { return m_readRetries; }
    QLabel *serialStatusLabel() const { return m_serialStatus; }
    QLabel *loginStatusLabel() const { return m_loginStatusLabel; }
    QLabel *createAdminStatusLabel() const { return m_createAdminStatus; }
    QLabel *userStatusLabel() const { return m_userStatus; }
    QLineEdit *changePasswordUserEdit() const { return m_changePasswordUser; }
    QLineEdit *changePasswordNewEdit() const { return m_changePasswordNew; }
    QLineEdit *changePasswordConfirmEdit() const { return m_changePasswordConfirm; }
    QPushButton *changePasswordButton() const { return m_changePassword; }
    QLabel *changePasswordStatusLabel() const { return m_changePasswordStatus; }
    QSpinBox *d122Spin() const { return m_d122Spin; }
    QSpinBox *d204Spin() const { return m_d204Spin; }
    QSpinBox *d220Spin() const { return m_d220Spin; }
    QPushButton *writeD122Button() const { return m_writeD122; }
    QPushButton *writeD204Button() const { return m_writeD204; }
    QPushButton *writeD220Button() const { return m_writeD220; }
    QPushButton *logoutButton() const { return m_logout; }
    QPushButton *createAdminButton() const { return m_createAdmin; }
    QLineEdit *adminUsernameEdit() const { return m_adminUsername; }
    QLineEdit *adminPasswordEdit() const { return m_adminPassword; }
    QLineEdit *adminConfirmEdit() const { return m_adminConfirm; }
    QLabel *d204WarningLabel() const { return m_d204Warning; }
    ValueDisplay *paramDisplay(const QString &key) const;
    QString paramStatusText() const;

    // --- data feeds (wired by the app shell, Task 20) --------------------------
    void setNeedsInitialAdmin(bool needs);
    void setInitialAdminResult(bool ok, const QString &detail);
    void setLoginResult(const LoginResult &result);
    void setSessionRemainingSec(int seconds);
    void setUsers(const QVector<UserRecord> &users);
    void setAddUserResult(bool ok, const QString &detail);
    void setDeleteUserResult(bool ok, const QString &detail);
    void setPasswordChangeResult(bool ok, const QString &detail);
    void setParameterWriteResult(bool ok, const QString &detail);
    // 回显实际存储的串口配置 (Task 20 接线 DatabaseService::getSetting).
    void setSerialConfig(const SerialConfig &config);
    // 串口配置保存结果 (Task 20 接线 DatabaseService::settingSaved + 重连).
    void setSerialSaveResult(bool ok, const QString &detail);

public slots:
    // Re-renders every widget from the model's current state.
    void refresh();

signals:
    // Request intents for the app shell (Task 20). Never emitted optimistically.
    void createInitialAdminRequested(const QString &username, const QString &password);
    void loginRequested(const QString &username, const QString &password);
    // 登录结果文本 (空 = 成功), 供登录对话框显示失败原因或关闭 (spec §11.5).
    void loginResultShown(const QString &text);
    void logoutRequested();
    // 注销触发 M42、M106-M111 清零流程 (spec §11.5).
    void logoutClearRequested();
    void addUserRequested(const QString &username, Role role, const QString &password);
    void changePasswordRequested(qint64 userId, const QString &newPassword);
    void deleteUserRequested(qint64 userId);
    void saveSerialConfigRequested(const SerialConfig &config);
    // D122/D220 参数写请求, 携带目标值 (D204 走 d204WriteRequested, 需二次验证).
    void writeParameterRequested(quint16 address, quint16 value);
    // D204 写请求, 携带管理员密码供 Task 20 二次验证 (spec §11.3).
    void d204WriteRequested(quint16 value, const QString &adminPassword);

private:
    void buildLayout();
    QWidget *buildLockedPanel();
    QWidget *buildCreateAdminPanel();
    QWidget *buildAdminPanel();
    QWidget *buildUserSection();
    QWidget *buildSerialSection();
    QWidget *buildParameterSection();
    ValueDisplay *addParamDisplay(const QString &key, const QString &title);
    void onLoginClicked();
    void onCreateAdminClicked();
    void onLogoutClicked();
    void onWriteD122();
    void onWriteD204();
    void onWriteD220();
    void onD204PasswordEntered(const QString &password);
    void onAddUserClicked();
    void onDeleteUserClicked();
    void onChangePasswordClicked();
    void onSaveSerialClicked();

    ShellModel &m_model;
    UsersSettingsModel m_pageModel;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_lockedPanel = nullptr;
    QWidget *m_createAdminPanel = nullptr;
    QWidget *m_adminPanel = nullptr;

    // Locked panel.
    QPushButton *m_loginButton = nullptr;
    // Create-admin panel.
    QLineEdit *m_adminUsername = nullptr;
    QLineEdit *m_adminPassword = nullptr;
    QLineEdit *m_adminConfirm = nullptr;
    QPushButton *m_createAdmin = nullptr;
    QLabel *m_createAdminStatus = nullptr;
    // Admin panel.
    QLabel *m_sessionLabel = nullptr;
    QLabel *m_loginStatusLabel = nullptr;
    QListWidget *m_userList = nullptr;
    QLineEdit *m_newUserName = nullptr;
    QComboBox *m_newUserRole = nullptr;
    QLineEdit *m_newUserPassword = nullptr;
    QPushButton *m_addUser = nullptr;
    QPushButton *m_deleteUser = nullptr;
    QLabel *m_userStatus = nullptr;
    QLineEdit *m_changePasswordUser = nullptr;
    QLineEdit *m_changePasswordNew = nullptr;
    QLineEdit *m_changePasswordConfirm = nullptr;
    QPushButton *m_changePassword = nullptr;
    QLabel *m_changePasswordStatus = nullptr;
    QLineEdit *m_comPort = nullptr;
    QSpinBox *m_station = nullptr;
    QComboBox *m_baudRate = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_parity = nullptr;
    QSpinBox *m_timeout = nullptr;
    QSpinBox *m_readRetries = nullptr;
    QPushButton *m_saveSerial = nullptr;
    QLabel *m_serialStatus = nullptr;
    QSpinBox *m_d122Spin = nullptr;
    QSpinBox *m_d204Spin = nullptr;
    QSpinBox *m_d220Spin = nullptr;
    QPushButton *m_writeD122 = nullptr;
    QPushButton *m_writeD204 = nullptr;
    QPushButton *m_writeD220 = nullptr;
    QLabel *m_d204Warning = nullptr;
    QLabel *m_paramStatus = nullptr;
    QPushButton *m_logout = nullptr;
    QHash<QString, ValueDisplay *> m_paramDisplays;

    QString m_accountStatus;

    bool m_sessionExpiredEmitted = false;
    QPointer<AdminPasswordDialog> m_d204Dialog;
};

} // namespace hlm
