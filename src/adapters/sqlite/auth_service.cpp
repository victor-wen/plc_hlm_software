#include "adapters/sqlite/auth_service.h"

#include <optional>

#include "domain/password_derivation.h"
#include "ports/repositories.h"

namespace hlm {

namespace {

// Builds a UserRecord with a freshly derived hash for `password`. Returns
// std::nullopt if the random salt or key derivation fails (e.g. OpenSSL
// RAND_bytes failure), so the caller can abort rather than store an empty hash.
std::optional<UserRecord> makeUserRecord(const QString &username, Role role,
                                         const QString &password)
{
    const QByteArray salt = generateSalt();
    if (salt.isEmpty())
        return std::nullopt;
    const QByteArray derived = derivePasswordKey(password, salt, kDefaultPbkdf2Iterations);
    if (derived.isEmpty())
        return std::nullopt;
    UserRecord u;
    u.username = username;
    u.role = role;
    u.salt = salt;
    u.iterations = kDefaultPbkdf2Iterations;
    u.passwordHash = passwordHashHex(derived);
    u.enabled = true;
    return u;
}

void audit(AuditRepository *audit, const QString &username, Role role,
           const QString &action, const QString &target, const QString &params,
           AuditResult result, const QString &reason)
{
    if (!audit)
        return;
    AuditRecord a;
    a.occurredAt = QDateTime::currentDateTimeUtc();
    a.username = username;
    a.role = role;
    a.action = action;
    a.target = target;
    a.redactedParameters = params;
    a.result = result;
    a.reason = reason;
    audit->append(a);
}

} // namespace

AuthService::AuthService(UserRepository *users, AuditRepository *audit,
                         SaltGeneratorFn saltGen, DeriveKeyFn derive)
    : m_users(users), m_audit(audit), m_saltGen(saltGen), m_derive(derive)
{
}

bool AuthService::needsInitialAdmin() const
{
    return m_users->countUsers() == 0;
}

bool AuthService::createInitialAdmin(const QString &username, const QString &password,
                                     QString *error)
{
    if (username.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("username must not be empty");
        return false;
    }
    if (password.isEmpty()) {
        if (error)
            *error = QStringLiteral("password must not be empty");
        return false;
    }
    if (m_users->countUsers() > 0) {
        if (error)
            *error = QStringLiteral("users already exist");
        return false;
    }
    const auto u = makeUserRecord(username, Role::Admin, password);
    if (!u) {
        if (error)
            *error = QStringLiteral("failed to derive password hash");
        return false;
    }
    if (!m_users->createUser(*u, error))
        return false;
    audit(m_audit, username, Role::Admin, QStringLiteral("user.create"),
          QStringLiteral("admin"), QString(), AuditResult::Success, QString());
    return true;
}

LoginResult AuthService::login(const QString &username, const QString &password)
{
    const auto user = m_users->findByName(username);
    if (!user) {
        // Do not reveal whether the account exists; still audit the attempt.
        // Run a dummy PBKDF2 derivation with the same iteration count so the
        // unknown-user path takes the same time as a wrong-password attempt
        // (timing side-channel: account existence must not be observable).
        const QByteArray dummySalt = m_saltGen();
        if (!dummySalt.isEmpty())
            m_derive(password, dummySalt, kDefaultPbkdf2Iterations);
        audit(m_audit, username, Role::Anonymous, QStringLiteral("auth.login"),
              QStringLiteral("user"), QString(), AuditResult::Failure,
              QStringLiteral("unknown user"));
        return {false, QStringLiteral("unknown user"), std::nullopt};
    }

    LockState &st = m_lockState[username];
    if (st.lockedUntil.isValid() && QDateTime::currentDateTimeUtc() < st.lockedUntil) {
        audit(m_audit, username, user->role, QStringLiteral("auth.login"),
              QStringLiteral("user"), QString(), AuditResult::Failure,
              QStringLiteral("locked"));
        return {false, QStringLiteral("locked"), std::nullopt};
    }
    // Lock window expired: reset the counter.
    if (st.lockedUntil.isValid())
        st = LockState{};

    if (!user->enabled) {
        audit(m_audit, username, user->role, QStringLiteral("auth.login"),
              QStringLiteral("user"), QString(), AuditResult::Failure,
              QStringLiteral("disabled"));
        return {false, QStringLiteral("disabled"), std::nullopt};
    }

    const QByteArray derived = derivePasswordKey(password, user->salt, user->iterations);
    const bool ok = !derived.isEmpty()
        && constantTimeEquals(passwordHashHex(derived), user->passwordHash);
    if (ok) {
        st = LockState{};
        audit(m_audit, username, user->role, QStringLiteral("auth.login"),
              QStringLiteral("user"), QString(), AuditResult::Success, QString());
        return {true, QString(), user};
    }

    ++st.failed;
    if (st.failed >= kMaxFailedLogins) {
        st.lockedUntil = QDateTime::currentDateTimeUtc().addSecs(m_lockoutSeconds);
        audit(m_audit, username, user->role, QStringLiteral("auth.login"),
              QStringLiteral("user"), QString(), AuditResult::Failure,
              QStringLiteral("locked"));
        return {false, QStringLiteral("locked"), std::nullopt};
    }
    audit(m_audit, username, user->role, QStringLiteral("auth.login"),
          QStringLiteral("user"), QString(), AuditResult::Failure,
          QStringLiteral("bad credentials"));
    return {false, QStringLiteral("bad credentials"), std::nullopt};
}

bool AuthService::changePassword(qint64 userId, const QString &newPassword,
                                 QString *error)
{
    if (newPassword.isEmpty()) {
        if (error)
            *error = QStringLiteral("password must not be empty");
        return false;
    }
    const QVector<UserRecord> users = m_users->allUsers();
    for (const UserRecord &u : users) {
        if (u.id == userId) {
            UserRecord updated = u;
            updated.salt = m_saltGen();
            if (updated.salt.isEmpty()) {
                if (error)
                    *error = QStringLiteral("failed to generate salt");
                return false;
            }
            updated.iterations = kDefaultPbkdf2Iterations;
            const QByteArray derived =
                m_derive(newPassword, updated.salt, updated.iterations);
            if (derived.isEmpty()) {
                if (error)
                    *error = QStringLiteral("failed to derive password hash");
                return false;
            }
            updated.passwordHash = passwordHashHex(derived);
            if (!m_users->updateUser(updated, error))
                return false;
            audit(m_audit, u.username, u.role, QStringLiteral("user.password_change"),
                  QStringLiteral("user"), QString(), AuditResult::Success, QString());
            return true;
        }
    }
    if (error)
        *error = QStringLiteral("user not found");
    return false;
}

bool AuthService::createUser(const QString &username, Role role,
                             const QString &password, QString *error)
{
    if (username.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("username must not be empty");
        return false;
    }
    if (password.isEmpty()) {
        if (error)
            *error = QStringLiteral("password must not be empty");
        return false;
    }
    if (m_users->findByName(username)) {
        if (error)
            *error = QStringLiteral("username already exists");
        return false;
    }
    const auto u = makeUserRecord(username, role, password);
    if (!u) {
        if (error)
            *error = QStringLiteral("failed to derive password hash");
        return false;
    }
    if (!m_users->createUser(*u, error))
        return false;
    audit(m_audit, username, role, QStringLiteral("user.create"),
          QStringLiteral("user"), QString(), AuditResult::Success, QString());
    return true;
}

bool AuthService::deleteUser(qint64 id, QString *error)
{
    if (!m_users->deleteUser(id, error))
        return false;
    audit(m_audit, QStringLiteral("admin"), Role::Admin, QStringLiteral("user.delete"),
          QStringLiteral("user"), QString(), AuditResult::Success, QString());
    return true;
}

} // namespace hlm
