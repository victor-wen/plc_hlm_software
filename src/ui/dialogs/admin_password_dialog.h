#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace hlm {

// 管理员密码二次验证对话框 (spec §11.3): D204 脉冲当量修改前要求再次输入
// 管理员密码. The password field never echoes plaintext and the dialog never
// logs or stores the password; the app shell (Task 20) verifies it.
class AdminPasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AdminPasswordDialog(const QString &prompt, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    QLineEdit *passwordEdit() const { return m_password; }
    QPushButton *okButton() const { return m_ok; }
    QPushButton *cancelButton() const { return m_cancel; }
    QLabel *statusLabel() const { return m_status; }

    void setStatus(const QString &text);

signals:
    void passwordEntered(const QString &password);

private:
    void onOkClicked();

    QLineEdit *m_password = nullptr;
    QPushButton *m_ok = nullptr;
    QPushButton *m_cancel = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace hlm
