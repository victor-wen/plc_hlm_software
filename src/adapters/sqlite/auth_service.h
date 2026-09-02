#pragma once

// Authentication service (spec §11.5).
//
// - First start forces creation of an admin account; no hardcoded default
//   password.
// - Passwords are verified with PBKDF2-HMAC-SHA256 (hlm_core); only the hex
//   digest of the derived key is stored.
// - 3 consecutive failed logins lock the account for 30 seconds; other
//   accounts are unaffected. Failed attempts are recorded in the audit log
//   WITHOUT the password.
//
// The service is a plain class used on the SQLite worker thread (spec §7.3).

#include <QDateTime>
#include <QHash>
#include <QString>

#include "ports/repositories.h"

namespace hlm {

inline constexpr int kMaxFailedLogins = 3;
inline constexpr int kLockoutSeconds = 30;

class UserRepository;
class AuditRepository;

class AuthService
{
public:
    AuthService(UserRepository *users, AuditRepository *audit);

    // True when no user exists yet (first start, spec §11.5).
    bool needsInitialAdmin() const;

    // Creates the initial admin account. `username` must be non-blank and
    // `password` must be non-empty. Returns false (with error) if a user
    // already exists, either input is empty, or key derivation fails.
    bool createInitialAdmin(const QString &username, const QString &password,
                            QString *error = nullptr);

    // Verifies credentials. On success returns the user record; on failure
    // returns a LoginResult with reason "unknown user", "disabled", "locked"
    // or "bad credentials". Failed attempts are audited; the password is never
    // written to the audit log.
    LoginResult login(const QString &username, const QString &password);

    // Changes a user's password (admin action). Returns false if the user does
    // not exist.
    bool changePassword(qint64 userId, const QString &newPassword,
                        QString *error = nullptr);

    // Overrides the lockout window (seconds). Test hook; default 30 (spec §11.5).
    void setLockoutSeconds(int seconds) { m_lockoutSeconds = seconds; }

private:
    struct LockState {
        int failed = 0;
        QDateTime lockedUntil;
    };

    UserRepository *m_users;
    AuditRepository *m_audit;
    int m_lockoutSeconds = kLockoutSeconds;
    QHash<QString, LockState> m_lockState; // in-memory, per-process
};

} // namespace hlm
