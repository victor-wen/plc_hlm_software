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
#include <QCheckBox>
#include <QStyle>
#include <QScrollArea>

namespace hlm {

namespace {
constexpr quint16 kD122 = 122; // 皮带速度
constexpr quint16 kD204 = 204; // 脉冲当量
constexpr quint16 kD220 = 220; // 调宽速度

QString friendlyAccountError(const QString &detail)
{
    if (detail == QStringLiteral("username must not be empty"))
        return QStringLiteral("用户名不能为空");
    if (detail == QStringLiteral("password must not be empty"))
        return QStringLiteral("密码不能为空");
    if (detail == QStringLiteral("username already exists"))
        return QStringLiteral("该用户名已存在");
    if (detail == QStringLiteral("users already exist"))
        return QStringLiteral("系统已存在用户，不能重复创建初始管理员");
    if (detail == QStringLiteral("user not found"))
        return QStringLiteral("未找到该用户");
    if (detail == QStringLiteral("cannot delete the last admin"))
        return QStringLiteral("不能删除系统中的最后一个管理员");
    if (detail == QStringLiteral("database restricted"))
        return QStringLiteral("数据库不可用，请检查数据目录权限");
    if (detail == QStringLiteral("failed to derive password hash"))
        return QStringLiteral("密码安全处理失败，请重试");
    return detail;
}

void showStatus(QLabel *label, const QString &text, bool ok)
{
    label->setText(text);
    label->setProperty("status", ok ? QStringLiteral("success")
                                     : QStringLiteral("error"));
    label->style()->unpolish(label);
    label->style()->polish(label);
}
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
    panel->setObjectName(QStringLiteral("lockedPanel"));
    auto *layout = new QVBoxLayout(panel);
    layout->addStretch();

    auto *card = new QFrame(panel);
    card->setObjectName(QStringLiteral("lockedCard"));
    card->setMaximumWidth(720);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(42, 34, 42, 38);
    cardLayout->setSpacing(12);

    auto *eyebrow = new QLabel(QStringLiteral("账户与权限"), card);
    eyebrow->setObjectName(QStringLiteral("setupEyebrow"));
    eyebrow->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(eyebrow);
    auto *icon = new QLabel(QStringLiteral("🔒"), card);
    icon->setObjectName(QStringLiteral("lockedIcon"));
    icon->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(icon);
    auto *title = new QLabel(QStringLiteral("需要管理员登录"), card);
    title->setObjectName(QStringLiteral("panelHeroTitle"));
    title->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(title);
    auto *hint = new QLabel(
        QStringLiteral("用户、通讯和参数设置仅管理员可用。请登录管理员账号后访问。"),
        card);
    hint->setObjectName(QStringLiteral("setupHint"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);
    // 登录失败/锁定原因显示在锁定面板上, 用户始终可见 (spec §11.5).
    m_loginStatusLabel = new QLabel(card);
    m_loginStatusLabel->setObjectName(QStringLiteral("loginStatus"));
    m_loginStatusLabel->setMinimumHeight(32);
    m_loginStatusLabel->setWordWrap(true);
    m_loginStatusLabel->setAlignment(Qt::AlignCenter);
    cardLayout->addWidget(m_loginStatusLabel);
    m_loginButton = new QPushButton(QStringLiteral("登录管理员账号"), card);
    m_loginButton->setObjectName(QStringLiteral("usersLoginButton"));
    m_loginButton->setMinimumHeight(48); // touch target >= 48 px (spec §11.1)
    m_loginButton->setMaximumWidth(280);
    cardLayout->addWidget(m_loginButton, 0, Qt::AlignHCenter);

    layout->addWidget(card, 0, Qt::AlignHCenter);
    layout->addStretch();
    connect(m_loginButton, &QPushButton::clicked, this,
            &UsersSettingsPage::onLoginClicked);
    return panel;
}

QWidget *UsersSettingsPage::buildCreateAdminPanel()
{
    // 首次启动强制创建管理员, 无默认密码 (spec §11.5).
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("createAdminPanel"));
    auto *layout = new QVBoxLayout(panel);
    layout->addStretch();

    auto *card = new QFrame(panel);
    card->setObjectName(QStringLiteral("bootstrapCard"));
    card->setMaximumWidth(760);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(42, 32, 42, 36);
    cardLayout->setSpacing(12);

    auto *eyebrow = new QLabel(QStringLiteral("首次配置 · 1 / 1"), card);
    eyebrow->setObjectName(QStringLiteral("setupEyebrow"));
    cardLayout->addWidget(eyebrow);
    auto *title = new QLabel(QStringLiteral("创建首位管理员"), card);
    title->setObjectName(QStringLiteral("panelHeroTitle"));
    cardLayout->addWidget(title);
    auto *hint = new QLabel(
        QStringLiteral("系统未检测到用户。该账号用于后续创建操作员、配置串口和修改设备参数。"),
        card);
    hint->setObjectName(QStringLiteral("setupHint"));
    hint->setWordWrap(true);
    cardLayout->addWidget(hint);

    auto *step = new QLabel(
        QStringLiteral("请自行设置用户名和密码；系统没有默认账号，也不会保存明文密码。"),
        card);
    step->setObjectName(QStringLiteral("setupStep"));
    step->setWordWrap(true);
    cardLayout->addWidget(step);

    auto *form = new QFormLayout();
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(10);
    m_adminUsername = new QLineEdit(card);
    m_adminUsername->setObjectName(QStringLiteral("adminUsername"));
    m_adminUsername->setMinimumHeight(48);
    m_adminUsername->setPlaceholderText(QStringLiteral("管理员用户名"));
    form->addRow(QStringLiteral("用户名"), m_adminUsername);
    m_adminPassword = new QLineEdit(card);
    m_adminPassword->setObjectName(QStringLiteral("adminPassword"));
    m_adminPassword->setMinimumHeight(48);
    m_adminPassword->setEchoMode(QLineEdit::Password); // 不回显明文 (spec §11.5)
    m_adminPassword->setPlaceholderText(QStringLiteral("密码"));
    form->addRow(QStringLiteral("密码"), m_adminPassword);
    m_adminConfirm = new QLineEdit(card);
    m_adminConfirm->setObjectName(QStringLiteral("adminConfirm"));
    m_adminConfirm->setMinimumHeight(48);
    m_adminConfirm->setEchoMode(QLineEdit::Password);
    m_adminConfirm->setPlaceholderText(QStringLiteral("再次输入密码"));
    form->addRow(QStringLiteral("确认密码"), m_adminConfirm);
    cardLayout->addLayout(form);

    auto *showPassword = new QCheckBox(QStringLiteral("显示密码"), card);
    connect(showPassword, &QCheckBox::toggled, this, [this](bool show) {
        const auto mode = show ? QLineEdit::Normal : QLineEdit::Password;
        m_adminPassword->setEchoMode(mode);
        m_adminConfirm->setEchoMode(mode);
    });
    cardLayout->addWidget(showPassword, 0, Qt::AlignRight);

    m_createAdminStatus = new QLabel(card);
    m_createAdminStatus->setObjectName(QStringLiteral("createAdminStatus"));
    m_createAdminStatus->setMinimumHeight(32);
    m_createAdminStatus->setWordWrap(true);
    cardLayout->addWidget(m_createAdminStatus);

    m_createAdmin = new QPushButton(QStringLiteral("创建管理员"), card);
    m_createAdmin->setObjectName(QStringLiteral("createAdminButton"));
    m_createAdmin->setMinimumHeight(48);
    m_createAdmin->setMaximumWidth(280);
    cardLayout->addWidget(m_createAdmin, 0, Qt::AlignRight);

    layout->addWidget(card, 0, Qt::AlignHCenter);
    layout->addStretch();
    connect(m_createAdmin, &QPushButton::clicked, this,
            &UsersSettingsPage::onCreateAdminClicked);
    connect(m_adminUsername, &QLineEdit::returnPressed, m_adminPassword,
            qOverload<>(&QWidget::setFocus));
    connect(m_adminPassword, &QLineEdit::returnPressed, m_adminConfirm,
            qOverload<>(&QWidget::setFocus));
    connect(m_adminConfirm, &QLineEdit::returnPressed, this,
            &UsersSettingsPage::onCreateAdminClicked);
    return panel;
}

QWidget *UsersSettingsPage::buildAdminPanel()
{
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("adminSettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto *panel = new QWidget(scroll);
    panel->setObjectName(QStringLiteral("adminSettingsContent"));
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
    scroll->setWidget(panel);
    return scroll;
}

QWidget *UsersSettingsPage::buildUserSection()
{
    auto *box = new QFrame(this);
    box->setObjectName(QStringLiteral("userPanel"));
    box->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(box);
    layout->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("用户管理"), box);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);

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

    m_userStatus = new QLabel(box);
    m_userStatus->setObjectName(QStringLiteral("userStatus"));
    m_userStatus->setMinimumHeight(32);
    m_userStatus->setWordWrap(true);
    layout->addWidget(m_userStatus);

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
    connect(m_userList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < m_pageModel.users().size())
            m_changePasswordUser->setText(m_pageModel.users().at(row).username);
    });
    return box;
}

QWidget *UsersSettingsPage::buildSerialSection()
{
    auto *box = new QFrame(this);
    box->setObjectName(QStringLiteral("serialPanel"));
    box->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(box);
    layout->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("串口配置"), box);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);

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
    auto *title = new QLabel(QStringLiteral("管理员参数"), box);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);

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
    if (needs)
        m_adminUsername->setFocus(Qt::OtherFocusReason);
}

void UsersSettingsPage::setInitialAdminResult(bool ok, const QString &detail)
{
    m_createAdmin->setEnabled(true);
    m_createAdmin->setText(QStringLiteral("创建管理员"));
    if (ok) {
        showStatus(m_createAdminStatus, QStringLiteral("管理员创建成功"), true);
        m_accountStatus = QStringLiteral("管理员已创建，请使用刚才设置的账号登录");
        m_adminPassword->clear();
        m_adminConfirm->clear();
    } else {
        showStatus(m_createAdminStatus,
                   QStringLiteral("创建失败：%1").arg(friendlyAccountError(detail)),
                   false);
    }
}

void UsersSettingsPage::setLoginResult(const LoginResult &result)
{
    m_pageModel.setLoginResult(result);
    if (result.ok)
        m_accountStatus.clear();
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

void UsersSettingsPage::setAddUserResult(bool ok, const QString &detail)
{
    m_addUser->setEnabled(true);
    showStatus(m_userStatus,
               ok ? QStringLiteral("用户创建成功")
                  : QStringLiteral("用户创建失败：%1")
                        .arg(friendlyAccountError(detail)),
               ok);
    if (ok) {
        m_newUserName->clear();
        m_newUserPassword->clear();
    }
}

void UsersSettingsPage::setDeleteUserResult(bool ok, const QString &detail)
{
    m_deleteUser->setEnabled(true);
    showStatus(m_userStatus,
               ok ? QStringLiteral("用户删除成功")
                  : QStringLiteral("用户删除失败：%1")
                        .arg(friendlyAccountError(detail)),
               ok);
}

void UsersSettingsPage::setPasswordChangeResult(bool ok, const QString &detail)
{
    m_changePassword->setEnabled(true);
    showStatus(m_changePasswordStatus,
               ok ? QStringLiteral("密码修改成功")
                  : QStringLiteral("密码修改失败：%1")
                        .arg(friendlyAccountError(detail)),
               ok);
    if (ok) {
        m_changePasswordNew->clear();
        m_changePasswordConfirm->clear();
    }
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
    m_saveSerial->setEnabled(true);
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
    if (!m_createAdmin->isEnabled())
        return;
    const QString username = m_adminUsername->text().trimmed();
    const QString password = m_adminPassword->text();
    const QString confirm = m_adminConfirm->text();
    if (username.isEmpty()) {
        showStatus(m_createAdminStatus, QStringLiteral("请输入管理员用户名"), false);
        m_adminUsername->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (password.isEmpty()) {
        showStatus(m_createAdminStatus, QStringLiteral("密码不能为空"), false);
        m_adminPassword->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (password != confirm) {
        showStatus(m_createAdminStatus, QStringLiteral("两次输入的密码不一致"), false);
        m_adminConfirm->selectAll();
        m_adminConfirm->setFocus(Qt::OtherFocusReason);
        return;
    }
    // 无默认密码: 只把用户输入的密码交给应用层 (spec §11.5).
    showStatus(m_createAdminStatus, QStringLiteral("正在安全创建管理员…"), true);
    m_createAdmin->setEnabled(false);
    m_createAdmin->setText(QStringLiteral("正在创建…"));
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
    const DeviceSnapshot &snapshot = m_model.snapshot();
    if (!m_model.snapshotFresh()
        || !snapshot.fieldValid(SnapshotField::PulsePerMm)) {
        m_paramStatus->setText(
            QStringLiteral("PLC 当前 D204 无效或已过期，无法校验参数组合"));
        return;
    }
    // Validate against the confirmed PLC counterpart, not an editor default.
    m_pageModel.setEditedD204(snapshot.pulsePerMm());
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
    const DeviceSnapshot &snapshot = m_model.snapshot();
    if (!m_model.snapshotFresh()
        || !snapshot.fieldValid(SnapshotField::WidthSpeed)) {
        m_paramStatus->setText(
            QStringLiteral("PLC 当前 D220 无效或已过期，无法校验参数组合"));
        return;
    }
    // Validate against the confirmed PLC counterpart, not an editor default.
    m_pageModel.setEditedD220(snapshot.widthSpeed());
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
    if (username.isEmpty()) {
        showStatus(m_userStatus, QStringLiteral("请输入新用户的用户名"), false);
        m_newUserName->setFocus(Qt::OtherFocusReason);
        return;
    }
    if (password.isEmpty()) {
        showStatus(m_userStatus, QStringLiteral("新用户密码不能为空"), false);
        m_newUserPassword->setFocus(Qt::OtherFocusReason);
        return;
    }
    const Role role = Role(m_newUserRole->currentData().toInt());
    showStatus(m_userStatus, QStringLiteral("正在创建用户…"), true);
    m_addUser->setEnabled(false);
    emit addUserRequested(username, role, password);
}

void UsersSettingsPage::onDeleteUserClicked()
{
    const int row = m_userList->currentRow();
    if (row < 0 || row >= m_pageModel.users().size())
        return;
    const UserRecord &target = m_pageModel.users().at(row);
    if (target.username == m_model.userName()) {
        showStatus(m_userStatus,
                   QStringLiteral("不能删除当前正在登录的账号，请先使用其他管理员登录"),
                   false);
        return;
    }
    // 删除用户需确认, 防止误删 (spec §11.5).
    const auto answer = QMessageBox::question(
        this, QStringLiteral("删除用户"),
        QStringLiteral("确定删除用户 \"%1\" 吗? 该操作不可撤销。")
            .arg(target.username),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    showStatus(m_userStatus, QStringLiteral("正在删除用户…"), true);
    m_deleteUser->setEnabled(false);
    emit deleteUserRequested(target.id);
}

void UsersSettingsPage::onChangePasswordClicked()
{
    const QString username = m_changePasswordUser->text().trimmed();
    const QString newPassword = m_changePasswordNew->text();
    const QString confirm = m_changePasswordConfirm->text();
    if (username.isEmpty()) {
        showStatus(m_changePasswordStatus,
                   QStringLiteral("请输入要改密的用户名"), false);
        return;
    }
    if (newPassword.isEmpty()) {
        showStatus(m_changePasswordStatus, QStringLiteral("新密码不能为空"), false);
        return;
    }
    if (newPassword != confirm) {
        showStatus(m_changePasswordStatus,
                   QStringLiteral("两次输入的新密码不一致"), false);
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
        showStatus(m_changePasswordStatus, QStringLiteral("未找到该用户"), false);
        return;
    }
    showStatus(m_changePasswordStatus, QStringLiteral("正在修改密码…"), true);
    m_changePassword->setEnabled(false);
    emit changePasswordRequested(userId, newPassword);
}

void UsersSettingsPage::onSaveSerialClicked()
{
    if (!m_saveSerial->isEnabled())
        return;
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
    m_saveSerial->setEnabled(false);
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
        const QString loginStatus = m_pageModel.loginStatusText();
        if (loginStatus.isEmpty() && !m_accountStatus.isEmpty())
            showStatus(m_loginStatusLabel, m_accountStatus, true);
        else
            showStatus(m_loginStatusLabel, loginStatus, false);
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
