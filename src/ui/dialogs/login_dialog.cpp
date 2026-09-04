#include "ui/dialogs/login_dialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QCheckBox>

namespace hlm {

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("loginDialog"));
    setWindowTitle(QStringLiteral("登录"));
    setModal(true);
    setMinimumWidth(480);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 24);
    root->setSpacing(16);

    auto *eyebrow = new QLabel(QStringLiteral("PLC · HMI CONTROL"), this);
    eyebrow->setObjectName(QStringLiteral("loginEyebrow"));
    root->addWidget(eyebrow);
    auto *title = new QLabel(QStringLiteral("登录控制系统"), this);
    title->setObjectName(QStringLiteral("loginTitle"));
    root->addWidget(title);
    auto *subtitle = new QLabel(
        QStringLiteral("使用已创建的操作员或管理员账号继续"), this);
    subtitle->setObjectName(QStringLiteral("loginSubtitle"));
    root->addWidget(subtitle);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("loginCard"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 20, 20, 20);
    cardLayout->setSpacing(12);

    auto *form = new QFormLayout();
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    m_username = new QLineEdit(this);
    m_username->setObjectName(QStringLiteral("loginUsername"));
    m_username->setMinimumHeight(48); // touch target >= 48 px (spec §11.1)
    m_username->setPlaceholderText(QStringLiteral("用户名"));
    form->addRow(QStringLiteral("用户名"), m_username);

    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("loginPassword"));
    m_password->setMinimumHeight(48);
    m_password->setEchoMode(QLineEdit::Password); // 不回显明文 (spec §11.5)
    m_password->setPlaceholderText(QStringLiteral("密码"));
    form->addRow(QStringLiteral("密码"), m_password);
    cardLayout->addLayout(form);

    auto *showPassword = new QCheckBox(QStringLiteral("显示密码"), card);
    showPassword->setObjectName(QStringLiteral("showLoginPassword"));
    connect(showPassword, &QCheckBox::toggled, this, [this](bool show) {
        m_password->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
    });
    cardLayout->addWidget(showPassword, 0, Qt::AlignRight);

    m_status = new QLabel(card);
    m_status->setObjectName(QStringLiteral("loginStatus"));
    m_status->setMinimumHeight(32);
    m_status->setWordWrap(true);
    cardLayout->addWidget(m_status);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch();
    m_cancel = new QPushButton(QStringLiteral("取消"), card);
    m_cancel->setObjectName(QStringLiteral("loginCancel"));
    m_cancel->setMinimumHeight(48);
    buttons->addWidget(m_cancel);
    m_ok = new QPushButton(QStringLiteral("登录"), card);
    m_ok->setObjectName(QStringLiteral("loginOk"));
    m_ok->setMinimumHeight(48);
    m_ok->setDefault(true);
    buttons->addWidget(m_ok);
    cardLayout->addLayout(buttons);
    root->addWidget(card);

    connect(m_ok, &QPushButton::clicked, this, &LoginDialog::onOkClicked);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_password, &QLineEdit::returnPressed, this, &LoginDialog::onOkClicked);
}

void LoginDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void LoginDialog::onOkClicked()
{
    const QString username = m_username->text().trimmed();
    if (username.isEmpty()) {
        m_status->setText(QStringLiteral("请输入用户名"));
        return;
    }
    // The password is passed to the app shell only; never logged or stored
    // here (spec §11.5: 失败审计不得记录密码).
    emit loginRequested(username, m_password->text());
}

} // namespace hlm
