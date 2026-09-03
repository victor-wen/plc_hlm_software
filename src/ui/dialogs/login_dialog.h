#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;

namespace hlm {

// 登录对话框 (spec §11.5). Collects username + password and emits
// loginRequested; the app shell (Task 20) wires it to
// DatabaseService::login. The password field never echoes plaintext
// (QLineEdit::Password) and the dialog never logs or stores the password.
class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    QLineEdit *usernameEdit() const { return m_username; }
    QLineEdit *passwordEdit() const { return m_password; }
    QPushButton *okButton() const { return m_ok; }
    QPushButton *cancelButton() const { return m_cancel; }
    QLabel *statusLabel() const { return m_status; }

    // Shows a login failure reason (e.g. "locked" / "bad credentials").
    void setStatus(const QString &text);

signals:
    void loginRequested(const QString &username, const QString &password);

private:
    void onOkClicked();

    QLineEdit *m_username = nullptr;
    QLineEdit *m_password = nullptr;
    QPushButton *m_ok = nullptr;
    QPushButton *m_cancel = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace hlm
