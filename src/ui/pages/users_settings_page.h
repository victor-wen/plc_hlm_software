#pragma once

#include <QWidget>
#include <QHash>
#include <QPointer>
#include <QVector>

#include "domain/serial_connection_settings.h"
#include "domain/serial_port_descriptor.h"
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
    QPushButton *saveSerialButton() const { return m_saveSerial; }

    // --- serial discovery / atomic save surface (PLC-HMI-004) ------------------
    QComboBox *serialPortComboBox() const { return m_serialPortCombo; }
    QLineEdit *serialPortEdit() const { return m_comPort; }
    QPushButton *saveSerialSettingsButton() const { return m_saveSerial; }
    QPushButton *refreshSerialPortsButton() const { return m_refreshSerialPorts; }
    QString serialSettingsStatusText() const;
    SerialConnectionSettings serialSettings() const;
    void setSerialSettings(const SerialConnectionSettings &settings);
    // Presents a passive discovery result: the saved port stays selected when
    // present and a missing saved port is reported, never silently replaced;
    // manual entry remains available in every case.
    void setDiscoveredSerialPorts(const QVector<SerialPortDescriptor> &ports);
    // Marks a save as in flight; a duplicate request is visibly rejected by the
    // page instead of being silently swallowed (spec NF-08).
    void setSerialSettingsSavePending();
    // Terminal batch result; `error` carries the database detail on failure.
    void setSerialSettingsSaveResult(bool committed, const QString &error);
    // Reports a failed passive enumeration without touching the settings.
    void setSerialSettingsEnumerationError(const QString &error);

    // Contract-named aliases (change PLC-HMI-004 public_api); the frozen
    // independent surface above is the primary spelling.
    void setDiscoveredPorts(const QVector<SerialPortDescriptor> &ports,
                            quint64 enumeration_request_id);
    quint64 lastEnumerationRequestId() const { return m_lastEnumerationRequestId; }

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
    // 扫码结果文件路径 (user decision 2026-09-22): the external scanning
    // program writes its results here. Empty = 未配置.
    QLineEdit *barcodePathEdit() const { return m_barcodePathEdit; }
    QPushButton *saveBarcodePathButton() const { return m_saveBarcodePath; }
    QString barcodePathStatusText() const;

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
    // Legacy spelling retained; the value type is the domain type (D1).
    void setSerialConfig(const SerialConnectionSettings &config);
    // 串口配置保存结果, 兼容旧调用方; 等价于 setSerialSettingsSaveResult.
    void setSerialSaveResult(bool ok, const QString &detail);
    // 扫码结果文件路径回显 (empty = 未配置) 与保存结果.
    void setBarcodeResultPath(const QString &path);
    void setBarcodePathSavePending();
    void setBarcodePathSaveResult(bool ok, const QString &detail);

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
    void saveSerialConfigRequested(const SerialConnectionSettings &config);
    // Frozen PLC-HMI-004 spellings: the save request carries the domain value
    // and the refresh is an explicit administrator action only.
    void saveSerialSettingsRequested(const SerialConnectionSettings &settings);
    void enumerateSerialPortsRequested();
    // Contract-named aliases; emitted through internal signal chaining.
    void refreshPortsRequested();
    // D122/D220 参数写请求, 携带目标值 (D204 走 d204WriteRequested, 需二次验证).
    void writeParameterRequested(quint16 address, quint16 value);
    // D204 写请求, 携带管理员密码供 Task 20 二次验证 (spec §11.3).
    void d204WriteRequested(quint16 value, const QString &adminPassword);
    // 扫码结果文件路径保存请求 (user decision 2026-09-22). Never optimistic:
    // the app shell reports the outcome through setBarcodePathSaveResult.
    void saveBarcodePathRequested(const QString &path);

private:
    void buildLayout();
    QWidget *buildLockedPanel();
    QWidget *buildCreateAdminPanel();
    QWidget *buildAdminPanel();
    QWidget *buildUserSection();
    QWidget *buildSerialSection();
    QWidget *buildParameterSection();
    // Adds a titled parameter value block to the caller's layout; returns the
    // wrapper widget, while the inner ValueDisplay stays available through
    // paramDisplay(key).
    QWidget *addParamDisplay(const QString &key, const QString &title);
    void onLoginClicked();
    void onCreateAdminClicked();
    void onLogoutClicked();
    void onWriteD122();
    void onWriteD204();
    void onWriteD220();
    void onSaveBarcodePathClicked();
    void onD204PasswordEntered(const QString &password);
    void onAddUserClicked();
    void onDeleteUserClicked();
    void onChangePasswordClicked();
    void onSaveSerialClicked();
    void onRefreshSerialPortsClicked();
    // Reads the widgets into a domain value and validates it via the page
    // model; used only by the explicit save action.
    SerialConnectionSettings serialSettingsFromWidgets() const;
    void applySerialSettingsToWidgets(const SerialConnectionSettings &settings);

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
    QComboBox *m_serialPortCombo = nullptr;
    QPushButton *m_refreshSerialPorts = nullptr;
    QSpinBox *m_station = nullptr;
    QComboBox *m_baudRate = nullptr;
    QComboBox *m_stopBits = nullptr;
    QComboBox *m_parity = nullptr;
    QSpinBox *m_timeout = nullptr;
    QSpinBox *m_readRetries = nullptr;
    QPushButton *m_saveSerial = nullptr;
    QLabel *m_serialStatus = nullptr;
    QVector<SerialPortDescriptor> m_discoveredPorts;
    bool m_serialSavePending = false;
    quint64 m_lastEnumerationRequestId = 0;
    QSpinBox *m_d122Spin = nullptr;
    QSpinBox *m_d204Spin = nullptr;
    QSpinBox *m_d220Spin = nullptr;
    // Last value this page seeded into each editor from a snapshot (or from the
    // authoritative default at construction). refresh() only re-renders an
    // editor whose current value is still this baseline, so an operator's
    // in-progress edit is never clobbered by a state-change reentrancy.
    int m_lastSeededD122 = 5000;
    int m_lastSeededD204 = 128;
    int m_lastSeededD220 = 15;
    QPushButton *m_writeD122 = nullptr;
    QPushButton *m_writeD204 = nullptr;
    QPushButton *m_writeD220 = nullptr;
    // 扫码结果文件路径 editor + its page-local result line.
    QLineEdit *m_barcodePathEdit = nullptr;
    QPushButton *m_saveBarcodePath = nullptr;
    QLabel *m_barcodePathStatus = nullptr;
    bool m_barcodePathSavePending = false;
    QLabel *m_d204Warning = nullptr;
    QLabel *m_paramStatus = nullptr;
    QPushButton *m_logout = nullptr;
    QHash<QString, ValueDisplay *> m_paramDisplays;

    QString m_accountStatus;

    bool m_sessionExpiredEmitted = false;
    QPointer<AdminPasswordDialog> m_d204Dialog;
};

} // namespace hlm
