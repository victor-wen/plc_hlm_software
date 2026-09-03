#include "ui/dialogs/admin_password_dialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace hlm {

AdminPasswordDialog::AdminPasswordDialog(const QString &prompt, QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("adminPasswordDialog"));
    setWindowTitle(QStringLiteral("管理员验证"));
    setModal(true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *promptLabel = new QLabel(prompt, this);
    promptLabel->setWordWrap(true);
    root->addWidget(promptLabel);

    m_password = new QLineEdit(this);
    m_password->setObjectName(QStringLiteral("adminPassword"));
    m_password->setMinimumHeight(48); // touch target >= 48 px (spec §11.1)
    m_password->setEchoMode(QLineEdit::Password); // 不回显明文 (spec §11.5)
    m_password->setPlaceholderText(QStringLiteral("管理员密码"));
    root->addWidget(m_password);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("adminPasswordStatus"));
    m_status->setMinimumHeight(32);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch();
    m_cancel = new QPushButton(QStringLiteral("取消"), this);
    m_cancel->setMinimumHeight(48);
    buttons->addWidget(m_cancel);
    m_ok = new QPushButton(QStringLiteral("确认"), this);
    m_ok->setObjectName(QStringLiteral("adminPasswordOk"));
    m_ok->setMinimumHeight(48);
    m_ok->setDefault(true);
    buttons->addWidget(m_ok);
    root->addLayout(buttons);

    connect(m_ok, &QPushButton::clicked, this, &AdminPasswordDialog::onOkClicked);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_password, &QLineEdit::returnPressed, this,
            &AdminPasswordDialog::onOkClicked);
}

void AdminPasswordDialog::setStatus(const QString &text)
{
    m_status->setText(text);
}

void AdminPasswordDialog::onOkClicked()
{
    if (m_password->text().isEmpty()) {
        m_status->setText(QStringLiteral("请输入管理员密码"));
        return;
    }
    // The password is passed to the app shell only; never logged or stored
    // here (spec §11.5: 失败审计不得记录密码).
    emit passwordEntered(m_password->text());
}

} // namespace hlm
