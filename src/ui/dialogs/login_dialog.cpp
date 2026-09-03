#include "ui/dialogs/login_dialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>

namespace hlm {

LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("loginDialog"));
    setWindowTitle(QStringLiteral("登录"));
    setModal(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *form = new QFormLayout();
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
    root->addLayout(form);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("loginStatus"));
    m_status->setMinimumHeight(32);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch();
    m_cancel = new QPushButton(QStringLiteral("取消"), this);
    m_cancel->setMinimumHeight(48);
    buttons->addWidget(m_cancel);
    m_ok = new QPushButton(QStringLiteral("登录"), this);
    m_ok->setObjectName(QStringLiteral("loginOk"));
    m_ok->setMinimumHeight(48);
    m_ok->setDefault(true);
    buttons->addWidget(m_ok);
    root->addLayout(buttons);

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
