#include "ui/pages/users_settings_page.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/value_display.h"
#include "ui/widgets/permission_button.h"
#include "ui/dialogs/login_dialog.h"
#include "ui/dialogs/admin_password_dialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QFrame>
#include <QMessageBox>

namespace hlm {

namespace {
constexpr quint16 kD122 = 122; // 皮带速度
constexpr quint16 kD204 = 204; // 脉冲当量
constexpr quint16 kD220 = 220; // 调宽速度
} // namespace

UsersSettingsPage::UsersSettingsPage(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_pageModel(model, this)
{
    setObjectName(QStringLiteral("usersSettingsPage"));
    buildLayout();
    connect(&m_model, &ShellModel::stateChanged, this, &UsersSettingsPage::refresh);
    connect(&m_pageModel, &UsersSettingsModel::stateChanged, this,
            &UsersSettingsPage::refresh);
    refresh();
}

void UsersSettingsPage::buildLayout()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    m_stack = new QStackedWidget(this);
    m_lockedPanel = buildLockedPanel();
    m_createAdminPanel = buildCreateAdminPanel();
    m_adminPanel = buildAdminPanel();
    m_stack->addWidget(m_lockedPanel);
    m_stack->addWidget(m_createAdminPanel);
    m_stack->addWidget(m_adminPanel);
    root->addWidget(m_stack, /*stretch=*/1);
}

QWidget *UsersSettingsPage::buildLockedPanel()
{
    // 未登录/操作员: 锁定面板, 不展示敏感字段 (spec §11.4).
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->addStretch();
    auto *icon = new QLabel(QStringLiteral("🔒"), panel);
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(QStringLiteral("font-size: 48px;"));
    layout->addWidget(icon);
    auto *title = new QLabel(QStringLiteral("需要管理员登录"), panel);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: bold;"));
    layout->addWidget(title);
    auto *hint = new QLabel(
        QStringLiteral("用户、通讯和参数设置仅管理员可用。请登录管理员账号后访问。"),
        panel);
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    // 登录失败/锁定原因显示在锁定面板上, 用户始终可见 (spec §11.5).
    m_loginStatusLabel = new QLabel(panel);
    m_loginStatusLabel->setObjectName(QStringLiteral("loginStatus"));
    m_loginStatusLabel->setMinimumHeight(32);
    m_loginStatusLabel->setWordWrap(true);
    m_loginStatusLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_loginStatusLabel);
    m_loginButton = new QPushButton(QStringLiteral("登录"), panel);
    m_loginButton->setObjectName(QStringLiteral("usersLoginButton"));
    m_loginButton->setMinimumHeight(48); // touch target >= 48 px (spec §11.1)
    m_loginButton->setMaximumWidth(240);
    layout->addWidget(m_loginButton, 0, Qt::AlignHCenter);
    layout->addStretch();
    connect(m_loginButton, &QPushButton::clicked, this,
            &UsersSettingsPage::onLoginClicked);
    return panel;
}

QWidget *UsersSettingsPage::buildCreateAdminPanel()
{
    // 首次启动强制创建管理员, 无默认密码 (spec §11.5).
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->addStretch();
    auto *title = new QLabel(QStringLiteral("首次启动: 创建管理员账号"), panel);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: bold;"));
    layout->addWidget(title);
    auto *hint = new QLabel(
        QStringLiteral("系统未检测到任何用户。请创建管理员账号, 密码不会提供默认值。"),
        panel);
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *form = new QFormLayout();
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(8);
    m_adminUsername = new QLineEdit(panel);
    m_adminUsername->setObjectName(QStringLiteral("adminUsername"));
    m_adminUsername->setMinimumHeight(48);
    m_adminUsername->setPlaceholderText(QStringLiteral("管理员用户名"));
    form->addRow(QStringLiteral("用户名"), m_adminUsername);
    m_adminPassword = new QLineEdit(panel);
    m_adminPassword->setObjectName(QStringLiteral("adminPassword"));
    m_adminPassword->setMinimumHeight(48);
    m_adminPassword->setEchoMode(QLineEdit::Password); // 不回显明文 (spec §11.5)
    m_adminPassword->setPlaceholderText(QStringLiteral("密码"));
    form->addRow(QStringLiteral("密码"), m_adminPassword);
    m_adminConfirm = new QLineEdit(panel);
    m_adminConfirm->setObjectName(QStringLiteral("adminConfirm"));
    m_adminConfirm->setMinimumHeight(48);
    m_adminConfirm->setEchoMode(QLineEdit::Password);
    m_adminConfirm->setPlaceholderText(QStringLiteral("再次输入密码"));
    form->addRow(QStringLiteral("确认密码"), m_adminConfirm);
    layout->addLayout(form);

    m_createAdmin = new QPushButton(QStringLiteral("创建管理员"), panel);
    m_createAdmin->setObjectName(QStringLiteral("createAdminButton"));
    m_createAdmin->setMinimumHeight(48);
    m_createAdmin->setMaximumWidth(240);
    layout->addWidget(m_createAdmin, 0, Qt::AlignHCenter);
    layout->addStretch();
    connect(m_createAdmin, &QPushButton::clicked, this,
            &UsersSettingsPage::onCreateAdminClicked);
    return panel;
}

QWidget *UsersSettingsPage::buildAdminPanel()
{
    auto *panel = new QWidget(this);
    auto *root = new QVBoxLayout(panel);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(12);

    // --- session + logout row ---------------------------------------------------
    auto *sessionRow = new QHBoxLayout();
    sessionRow->setSpacing(12);
    m_sessionLabel = new QLabel(panel);
    m_sessionLabel->setObjectName(QStringLiteral("sessionCountdown"));
    m_sessionLabel->setMinimumHeight(48);
    sessionRow->addWidget(m_sessionLabel, /*stretch=*/1);
    m_logout = new QPushButton(QStringLiteral("注销"), panel);
    m_logout->setObjectName(QStringLiteral("logoutButton"));
    m_logout->setMinimumHeight(48);
    sessionRow->addWidget(m_logout);
    root->addLayout(sessionRow);

    auto *columns = new QHBoxLayout();
    columns->setSpacing(12);
    columns->addWidget(buildUserSection(), /*stretch=*/1);
    columns->addWidget(buildSerialSection(), /*stretch=*/1);
    columns->addWidget(buildParameterSection(), /*stretch=*/1);
    root->addLayout(columns, /*stretch=*/1);

    connect(m_logout, &QPushButton::clicked, this, &UsersSettingsPage::onLogoutClicked);
    return panel;
}

QWidget *UsersSettingsPage::buildUserSection()
{
    auto *box = new QFrame(this);
    box->setObjectName(QStringLiteral("userPanel"));
    box->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(box);
    layout->setSpacing(8);
    layout->addWidget(new QLabel(QStringLiteral("用户管理"), box));

    m_userList = new QListWidget(box);
    m_userList->setObjectName(QStringLiteral("userList"));
    m_userList->setMinimumHeight(160);
    m_userList->setUniformItemSizes(true);
    // 行高 >= 48 px, 触摸目标达标 (spec §11.1).
    m_userList->setStyleSheet(QStringLiteral("QListWidget::item { min-height: 48px; }"));
    layout->addWidget(m_userList);

    auto *form = new QFormLayout();
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(6);
    m_newUserName = new QLineEdit(box);
    m_newUserName->setObjectName(QStringLiteral("newUserName"));
    m_newUserName->setMinimumHeight(48);
    m_newUserName->setPlaceholderText(QStringLiteral("用户名"));
    form->addRow(QStringLiteral("用户名"), m_newUserName);
    m_newUserRole = new QComboBox(box);
    m_newUserRole->setObjectName(QStringLiteral("newUserRole"));
    m_newUserRole->setMinimumHeight(48);
    m_newUserRole->addItem(QStringLiteral("操作员"), int(Role::Operator));
    m_newUserRole->addItem(QStringLiteral("管理员"), int(Role::Admin));
    form->addRow(QStringLiteral("角色"), m_newUserRole);
    m_newUserPassword = new QLineEdit(box);
    m_newUserPassword->setObjectName(QStringLiteral("newUserPassword"));
    m_newUserPassword->setMinimumHeight(48);
    m_newUserPassword->setEchoMode(QLineEdit::Password); // 不回显明文 (spec §11.5)
    m_newUserPassword->setPlaceholderText(QStringLiteral("密码"));
    form->addRow(QStringLiteral("密码"), m_newUserPassword);
    layout->addLayout(form);

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    m_addUser = new QPushButton(QStringLiteral("新增用户"), box);
    m_addUser->setObjectName(QStringLiteral("addUserButton"));
    m_addUser->setMinimumHeight(48);
    buttons->addWidget(m_addUser);
    m_deleteUser = new QPushButton(QStringLiteral("删除用户"), box);
    m_deleteUser->setObjectName(QStringLiteral("deleteUserButton"));
    m_deleteUser->setMinimumHeight(48);
    buttons->addWidget(m_deleteUser);
    buttons->addStretch();
    layout->addLayout(buttons);

    // 改密 (brief: 用户增删改密): 选择用户 -> 新密码 -> 确认密码, 密码框不回显明文.
    auto *changeForm = new QFormLayout();
    changeForm->setHorizontalSpacing(8);
    changeForm->setVerticalSpacing(6);
    m_changePasswordUser = new QLineEdit(box);
    m_changePasswordUser->setObjectName(QStringLiteral("changePasswordUser"));
    m_changePasswordUser->setMinimumHeight(48);
    m_changePasswordUser->setPlaceholderText(QStringLiteral("用户名"));
    changeForm->addRow(QStringLiteral("用户"), m_changePasswordUser);
    m_changePasswordNew = new QLineEdit(box);
    m_changePasswordNew->setObjectName(QStringLiteral("changePasswordNew"));
    m_changePasswordNew->setMinimumHeight(48);
    m_changePasswordNew->setEchoMode(QLineEdit::Password); // 不回显明文 (spec §11.5)
    m_changePasswordNew->setPlaceholderText(QStringLiteral("新密码"));
    changeForm->addRow(QStringLiteral("新密码"), m_changePasswordNew);
    m_changePasswordConfirm = new QLineEdit(box);
    m_changePasswordConfirm->setObjectName(QStringLiteral("changePasswordConfirm"));
    m_changePasswordConfirm->setMinimumHeight(48);
    m_changePasswordConfirm->setEchoMode(QLineEdit::Password);
    m_changePasswordConfirm->setPlaceholderText(QStringLiteral("确认新密码"));
    changeForm->addRow(QStringLiteral("确认"), m_changePasswordConfirm);
    layout->addLayout(changeForm);

    m_changePassword = new QPushButton(QStringLiteral("修改密码"), box);
    m_changePassword->setObjectName(QStringLiteral("changePasswordButton"));
    m_changePassword->setMinimumHeight(48);
    layout->addWidget(m_changePassword);
    m_changePasswordStatus = new QLabel(box);
    m_changePasswordStatus->setObjectName(QStringLiteral("changePasswordStatus"));
    m_changePasswordStatus->setMinimumHeight(32);
    m_changePasswordStatus->setWordWrap(true);
    layout->addWidget(m_changePasswordStatus);

    connect(m_addUser, &QPushButton::clicked, this, &UsersSettingsPage::onAddUserClicked);
    connect(m_deleteUser, &QPushButton::clicked, this,
            &UsersSettingsPage::onDeleteUserClicked);
    connect(m_changePassword, &QPushButton::clicked, this,
            &UsersSettingsPage::onChangePasswordClicked);
    return box;
}

QWidget *UsersSettingsPage::buildSerialSection()
{
    auto *box = new QFrame(this);
    box->setObjectName(QStringLiteral("serialPanel"));
    box->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(box);
    layout->setSpacing(8);
    layout->addWidget(new QLabel(QStringLiteral("串口配置"), box));

    auto *form = new QFormLayout();
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(6);
    m_comPort = new QLineEdit(box);
    m_comPort->setObjectName(QStringLiteral("comPort"));
    m_comPort->setMinimumHeight(48);
    m_comPort->setText(QStringLiteral("COM1"));
    form->addRow(QStringLiteral("COM 口"), m_comPort);
    m_station = new QSpinBox(box);
    m_station->setObjectName(QStringLiteral("station"));
    m_station->setRange(1, 247); // 站号 1-247 (spec §8.1)
    m_station->setValue(1);
    m_station->setMinimumHeight(48);
    form->addRow(QStringLiteral("站号"), m_station);
    m_baudRate = new QComboBox(box);
    m_baudRate->setObjectName(QStringLiteral("baudRate"));
    m_baudRate->setMinimumHeight(48);
    m_baudRate->addItem(QStringLiteral("9600"), 9600);
    m_baudRate->addItem(QStringLiteral("19200"), 19200);
    form->addRow(QStringLiteral("波特率"), m_baudRate);
    m_stopBits = new QComboBox(box);
    m_stopBits->setObjectName(QStringLiteral("stopBits"));
    m_stopBits->setMinimumHeight(48);
    m_stopBits->addItem(QStringLiteral("1"), 1);
    m_stopBits->addItem(QStringLiteral("2"), 2);
    form->addRow(QStringLiteral("停止位"), m_stopBits);
    m_parity = new QComboBox(box);
    m_parity->setObjectName(QStringLiteral("parity"));
    m_parity->setMinimumHeight(48);
    m_parity->addItem(QStringLiteral("无"));
    m_parity->addItem(QStringLiteral("奇"));
    m_parity->addItem(QStringLiteral("偶"));
    form->addRow(QStringLiteral("校验"), m_parity);
    m_timeout = new QSpinBox(box);
    m_timeout->setObjectName(QStringLiteral("timeoutMs"));
    m_timeout->setRange(50, 5000);
    m_timeout->setValue(200);
    m_timeout->setSuffix(QStringLiteral(" ms"));
    m_timeout->setMinimumHeight(48);
    form->addRow(QStringLiteral("超时"), m_timeout);
    m_readRetries = new QSpinBox(box);
    m_readRetries->setObjectName(QStringLiteral("readRetries"));
    m_readRetries->setRange(0, 5);
    m_readRetries->setValue(1);
    m_readRetries->setMinimumHeight(48);
    form->addRow(QStringLiteral("读重试"), m_readRetries);
    layout->addLayout(form);

    m_serialStatus = new QLabel(box);
    m_serialStatus->setObjectName(QStringLiteral("serialStatus"));
    m_serialStatus->setMinimumHeight(32);
    m_serialStatus->setWordWrap(true);
    layout->addWidget(m_serialStatus);

    m_saveSerial = new QPushButton(QStringLiteral("保存并重连"), box);
    m_saveSerial->setObjectName(QStringLiteral("saveSerialButton"));
    m_saveSerial->setMinimumHeight(48);
    layout->addWidget(m_saveSerial);
    connect(m_saveSerial, &QPushButton::clicked, this,
            &UsersSettingsPage::onSaveSerialClicked);
    return box;
}

QWidget *UsersSettingsPage::buildParameterSection()
{
    auto *box = new QFrame(this);
    box->setObjectName(QStringLiteral("parameterPanel"));
    box->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(box);
    layout->setSpacing(8);
    layout->addWidget(new QLabel(QStringLiteral("管理员参数"), box));

    // 参数值显示来自快照 (D122/D204/D220), 无乐观更新 (spec §11.2).
    layout->addWidget(addParamDisplay(QStringLiteral("d122"),
                                      QStringLiteral("皮带速度 (D122)")));
    layout->addWidget(addParamDisplay(QStringLiteral("d204"),
                                      QStringLiteral("脉冲当量 (D204)")));
    layout->addWidget(addParamDisplay(QStringLiteral("d220"),
                                      QStringLiteral("调宽速度 (D220)")));

    // D204 脉冲当量必须单独显示"非专业人员勿修改" (spec §11.3).
    m_d204Warning = new QLabel(QStringLiteral("⚠ 非专业人员勿修改"), box);
    m_d204Warning->setObjectName(QStringLiteral("d204Warning"));
    m_d204Warning->setMinimumHeight(32);
    m_d204Warning->setStyleSheet(
        QStringLiteral("color: #c42b2b; font-weight: bold;"));
    layout->addWidget(m_d204Warning);

    auto *form = new QFormLayout();
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(6);
    m_d122Spin = new QSpinBox(box);
    m_d122Spin->setObjectName(QStringLiteral("d122Spin"));
    m_d122Spin->setRange(100, 20000); // D122 100-20000 Hz
    m_d122Spin->setValue(1000);
    m_d122Spin->setSuffix(QStringLiteral(" Hz"));
    m_d122Spin->setMinimumHeight(48);
    form->addRow(QStringLiteral("D122"), m_d122Spin);
    m_d204Spin = new QSpinBox(box);
    m_d204Spin->setObjectName(QStringLiteral("d204Spin"));
    m_d204Spin->setRange(1, 32767); // D204 1-32767 脉冲/mm
    m_d204Spin->setValue(1280);
    m_d204Spin->setSuffix(QStringLiteral(" 脉冲/mm"));
    m_d204Spin->setMinimumHeight(48);
    form->addRow(QStringLiteral("D204"), m_d204Spin);
    m_d220Spin = new QSpinBox(box);
    m_d220Spin->setObjectName(QStringLiteral("d220Spin"));
    m_d220Spin->setRange(1, 15); // D220 1-15 mm/s
    m_d220Spin->setValue(2);
    m_d220Spin->setSuffix(QStringLiteral(" mm/s"));
    m_d220Spin->setMinimumHeight(48);
    form->addRow(QStringLiteral("D220"), m_d220Spin);
    layout->addLayout(form);

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(8);
    m_writeD122 = new QPushButton(QStringLiteral("写入 D122"), box);
    m_writeD122->setObjectName(QStringLiteral("writeD122Button"));
    m_writeD122->setMinimumHeight(48);
    buttons->addWidget(m_writeD122);
    m_writeD204 = new QPushButton(QStringLiteral("写入 D204"), box);
    m_writeD204->setObjectName(QStringLiteral("writeD204Button"));
    m_writeD204->setMinimumHeight(48);
    buttons->addWidget(m_writeD204);
    m_writeD220 = new QPushButton(QStringLiteral("写入 D220"), box);
    m_writeD220->setObjectName(QStringLiteral("writeD220Button"));
    m_writeD220->setMinimumHeight(48);
    buttons->addWidget(m_writeD220);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_paramStatus = new QLabel(box);
    m_paramStatus->setObjectName(QStringLiteral("paramStatus"));
    m_paramStatus->setMinimumHeight(32);
    m_paramStatus->setWordWrap(true);
    layout->addWidget(m_paramStatus);

    connect(m_writeD122, &QPushButton::clicked, this, &UsersSettingsPage::onWriteD122);
    connect(m_writeD204, &QPushButton::clicked, this, &UsersSettingsPage::onWriteD204);
    connect(m_writeD220, &QPushButton::clicked, this, &UsersSettingsPage::onWriteD220);
    return box;
}

ValueDisplay *UsersSettingsPage::addParamDisplay(const QString &key,
                                                 const QString &title)
{
    auto *wrap = new QWidget(this);
    auto *layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(new QLabel(title, wrap));
    auto *display = new ValueDisplay(wrap);
    display->setMinimumHeight(48);
    layout->addWidget(display);
    m_paramDisplays.insert(key, display);
    return display;
}

ValueDisplay *UsersSettingsPage::paramDisplay(const QString &key) const
{
    return m_paramDisplays.value(key, nullptr);
}

QWidget *UsersSettingsPage::currentPanel() const
{
    return m_stack ? m_stack->currentWidget() : nullptr;
}

QString UsersSettingsPage::paramStatusText() const
{
    return m_paramStatus ? m_paramStatus->text() : QString();
}

// --- data feeds -------------------------------------------------------------------

void UsersSettingsPage::setNeedsInitialAdmin(bool needs)
{
    m_pageModel.setNeedsInitialAdmin(needs);
    refresh();
}

void UsersSettingsPage::setLoginResult(const LoginResult &result)
{
    m_pageModel.setLoginResult(result);
    // 登录对话框: 失败显示原因, 成功关闭 (spec §11.5).
    emit loginResultShown(m_pageModel.loginStatusText());
    refresh();
}

void UsersSettingsPage::setSessionRemainingSec(int seconds)
{
    m_pageModel.setSessionRemainingSec(seconds);
    // 会话恢复 (二次登录后重新计时): 复位超时标记, 下次超时仍会注销+清零.
    if (seconds > 0)
        m_sessionExpiredEmitted = false;
    // 会话超时: 发出注销 + M42/M106-M111 清零请求, 只发一次 (spec §11.5).
    if (m_pageModel.sessionExpired() && !m_sessionExpiredEmitted) {
        m_sessionExpiredEmitted = true;
        emit logoutRequested();
        emit logoutClearRequested();
    }
    refresh();
}

void UsersSettingsPage::setUsers(const QVector<UserRecord> &users)
{
    m_pageModel.setUsers(users);
    m_userList->clear();
    for (const UserRecord &u : m_pageModel.users()) {
        const QString roleText = u.role == Role::Admin
            ? QStringLiteral("管理员")
            : QStringLiteral("操作员");
        auto *item = new QListWidgetItem(
            QStringLiteral("%1 (%2)").arg(u.username, roleText), m_userList);
        // 行高 >= 48 px, 触摸目标达标 (spec §11.1).
        item->setSizeHint(QSize(0, 48));
    }
    refresh();
}

void UsersSettingsPage::setParameterWriteResult(bool ok, const QString &detail)
{
    m_pageModel.setParameterWriteResult(ok, detail);
    refresh();
}

void UsersSettingsPage::setSerialConfig(const SerialConfig &config)
{
    m_pageModel.setSerialConfig(config);
    // 回显实际存储的串口配置 (Task 20 接线 DatabaseService::getSetting).
    m_comPort->setText(config.comPort);
    m_station->setValue(config.station);
    const int baudIdx = m_baudRate->findData(config.baudRate);
    if (baudIdx >= 0)
        m_baudRate->setCurrentIndex(baudIdx);
    const int stopIdx = m_stopBits->findData(config.stopBits);
    if (stopIdx >= 0)
        m_stopBits->setCurrentIndex(stopIdx);
    const int parityIdx = m_parity->findText(config.parity);
    if (parityIdx >= 0)
        m_parity->setCurrentIndex(parityIdx);
    m_timeout->setValue(config.timeoutMs);
    m_readRetries->setValue(config.readRetries);
    refresh();
}

void UsersSettingsPage::setSerialSaveResult(bool ok, const QString &detail)
{
    // 非乐观状态: 保存结果由 Task 20 回填 (spec §11.2).
    m_serialStatus->setText(ok ? QStringLiteral("串口配置已保存并重连")
                               : QStringLiteral("串口配置保存失败: %1").arg(detail));
    refresh();
}

// --- actions ----------------------------------------------------------------------

void UsersSettingsPage::onLoginClicked()
{
    auto *dialog = new LoginDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    // The app shell (Task 20) verifies the credentials and feeds the result
    // back via setLoginResult; the dialog itself never logs the password.
    // 不立即 accept: 失败原因显示在对话框内, 用户可重试 (spec §11.5).
    connect(dialog, &LoginDialog::loginRequested, this,
            [this, dialog](const QString &username, const QString &password) {
                emit loginRequested(username, password);
            });
    connect(this, &UsersSettingsPage::loginResultShown, dialog,
            [dialog](const QString &text) {
                if (text.isEmpty())
                    dialog->accept();
                else
                    dialog->setStatus(text);
            });
    dialog->open();
}

void UsersSettingsPage::onCreateAdminClicked()
{
    const QString username = m_adminUsername->text().trimmed();
    const QString password = m_adminPassword->text();
    const QString confirm = m_adminConfirm->text();
    if (username.isEmpty()) {
        m_adminUsername->setPlaceholderText(QStringLiteral("请输入用户名"));
        return;
    }
    if (password.isEmpty()) {
        m_adminPassword->setPlaceholderText(QStringLiteral("密码不能为空"));
        return;
    }
    if (password != confirm) {
        m_adminConfirm->setPlaceholderText(QStringLiteral("两次输入的密码不一致"));
        return;
    }
    // 无默认密码: 只把用户输入的密码交给应用层 (spec §11.5).
    emit createInitialAdminRequested(username, password);
}

void UsersSettingsPage::onLogoutClicked()
{
    // 注销触发 M42、M106-M111 清零流程 (spec §11.5).
    emit logoutRequested();
    emit logoutClearRequested();
}

void UsersSettingsPage::onWriteD122()
{
    m_pageModel.setEditedD122(m_d122Spin->value());
    if (!m_pageModel.d122Valid()) {
        m_paramStatus->setText(m_pageModel.paramReasons().join(QStringLiteral("；")));
        return;
    }
    // 无乐观状态: 只标记等待, 成功/失败来自 setParameterWriteResult (spec §11.2).
    m_pageModel.setParameterWritePending();
    emit writeParameterRequested(kD122, quint16(m_d122Spin->value()));
    refresh();
}

void UsersSettingsPage::onWriteD220()
{
    m_pageModel.setEditedD220(m_d220Spin->value());
    if (!m_pageModel.d220Valid() || !m_pageModel.productValid()) {
        m_paramStatus->setText(m_pageModel.paramReasons().join(QStringLiteral("；")));
        return;
    }
    m_pageModel.setParameterWritePending();
    emit writeParameterRequested(kD220, quint16(m_d220Spin->value()));
    refresh();
}

void UsersSettingsPage::onWriteD204()
{
    m_pageModel.setEditedD204(m_d204Spin->value());
    if (!m_pageModel.d204Valid() || !m_pageModel.productValid()) {
        m_paramStatus->setText(m_pageModel.paramReasons().join(QStringLiteral("；")));
        return;
    }
    // D204 修改需再次输入管理员密码 (spec §11.3): 二次验证对话框.
    // QPointer: 对话框 WA_DeleteOnClose 后自动置空, 连点不会叠多个对话框.
    if (m_d204Dialog) {
        m_d204Dialog->raise();
        m_d204Dialog->activateWindow();
        return;
    }
    m_d204Dialog = new AdminPasswordDialog(
        QStringLiteral("修改 D204 脉冲当量需要再次验证管理员密码。"), this);
    m_d204Dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_d204Dialog, &AdminPasswordDialog::passwordEntered, this,
            &UsersSettingsPage::onD204PasswordEntered);
    m_d204Dialog->open();
}

void UsersSettingsPage::onD204PasswordEntered(const QString &password)
{
    // 密码随写请求交给应用层 (Task 20) 验证, 页面不记录 (spec §11.5).
    emit d204WriteRequested(quint16(m_d204Spin->value()), password);
    if (m_d204Dialog)
        m_d204Dialog->accept();
}

void UsersSettingsPage::onAddUserClicked()
{
    const QString username = m_newUserName->text().trimmed();
    const QString password = m_newUserPassword->text();
    if (username.isEmpty() || password.isEmpty())
        return;
    const Role role = Role(m_newUserRole->currentData().toInt());
    emit addUserRequested(username, role, password);
    m_newUserName->clear();
    m_newUserPassword->clear();
}

void UsersSettingsPage::onDeleteUserClicked()
{
    const int row = m_userList->currentRow();
    if (row < 0 || row >= m_pageModel.users().size())
        return;
    const UserRecord &target = m_pageModel.users().at(row);
    // 删除用户需确认, 防止误删 (spec §11.5).
    const auto answer = QMessageBox::question(
        this, QStringLiteral("删除用户"),
        QStringLiteral("确定删除用户 \"%1\" 吗? 该操作不可撤销。")
            .arg(target.username),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    emit deleteUserRequested(target.id);
}

void UsersSettingsPage::onChangePasswordClicked()
{
    const QString username = m_changePasswordUser->text().trimmed();
    const QString newPassword = m_changePasswordNew->text();
    const QString confirm = m_changePasswordConfirm->text();
    if (username.isEmpty()) {
        m_changePasswordStatus->setText(QStringLiteral("请输入要改密的用户名"));
        return;
    }
    if (newPassword.isEmpty()) {
        m_changePasswordStatus->setText(QStringLiteral("新密码不能为空"));
        return;
    }
    if (newPassword != confirm) {
        m_changePasswordStatus->setText(QStringLiteral("两次输入的新密码不一致"));
        return;
    }
    // 按用户名匹配用户 (改密对象来自用户列表, 不回显明文, spec §11.5).
    qint64 userId = -1;
    for (const UserRecord &u : m_pageModel.users()) {
        if (u.username == username) {
            userId = u.id;
            break;
        }
    }
    if (userId < 0) {
        m_changePasswordStatus->setText(QStringLiteral("未找到该用户"));
        return;
    }
    emit changePasswordRequested(userId, newPassword);
    m_changePasswordStatus->setText(QStringLiteral("已请求修改密码"));
    m_changePasswordNew->clear();
    m_changePasswordConfirm->clear();
}

void UsersSettingsPage::onSaveSerialClicked()
{
    SerialConfig cfg;
    cfg.comPort = m_comPort->text().trimmed();
    cfg.station = m_station->value();
    cfg.baudRate = m_baudRate->currentData().toInt();
    cfg.stopBits = m_stopBits->currentData().toInt();
    cfg.parity = m_parity->currentText();
    cfg.timeoutMs = m_timeout->value();
    cfg.readRetries = m_readRetries->value();
    m_pageModel.setSerialConfig(cfg);
    if (!m_pageModel.serialConfigValid()) {
        m_serialStatus->setText(
            m_pageModel.serialConfigReasons().join(QStringLiteral("；")));
        return;
    }
    // 修改通讯配置必须断开后重连并写入审计 (spec §8.1): 页面只发请求信号,
    // Task 20 接线 DatabaseService::setSetting + 重连 + 审计.
    emit saveSerialConfigRequested(cfg);
    // 非乐观状态: 只标记等待确认, 结果由 Task 20 的 feed 接口回填.
    m_serialStatus->setText(QStringLiteral("等待确认保存并重连"));
}

// --- rendering ---------------------------------------------------------------------

void UsersSettingsPage::refresh()
{
    // Panel selection: 首次启动创建管理员 > 管理员面板 > 锁定面板.
    if (m_pageModel.needsInitialAdmin()) {
        m_stack->setCurrentWidget(m_createAdminPanel);
        return;
    }
    if (m_pageModel.isAdmin()) {
        m_stack->setCurrentWidget(m_adminPanel);
    } else {
        m_stack->setCurrentWidget(m_lockedPanel);
        // 登录失败/锁定原因显示在锁定面板上 (spec §11.5).
        m_loginStatusLabel->setText(m_pageModel.loginStatusText());
        return;
    }

    // Session countdown (spec §11.5).
    m_sessionLabel->setText(m_pageModel.sessionStatusText());

    // Parameter values from the snapshot (spec §11.2: 无乐观更新).
    const DeviceSnapshot &s = m_model.snapshot();
    const bool fresh = m_model.snapshotFresh();
    const auto field = [&](const QString &key, const QString &text,
                           SnapshotField f, const QString &unit) {
        const bool valid = fresh && s.fieldValid(f);
        m_paramDisplays[key]->setValue(valid ? text : QString(), unit, valid);
    };
    field(QStringLiteral("d122"), QString::number(s.beltSpeed()),
          SnapshotField::BeltSpeed, QStringLiteral("Hz"));
    field(QStringLiteral("d204"), QString::number(s.pulsePerMm()),
          SnapshotField::PulsePerMm, QStringLiteral("脉冲/mm"));
    field(QStringLiteral("d220"), QString::number(s.widthSpeed()),
          SnapshotField::WidthSpeed, QStringLiteral("mm/s"));

    // Parameter write status.
    m_paramStatus->setText(m_pageModel.paramStatusText());
}

} // namespace hlm
