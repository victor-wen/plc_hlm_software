// Task 8 unit tests: authentication (spec §11.5).
// Covers: first-start admin creation (no default password), PBKDF2 storage
// (no plaintext), 3-failure lockout for 30 s, other accounts unaffected,
// disabled users, and audit records without passwords.

#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "adapters/sqlite/auth_service.h"
#include "adapters/sqlite/sqlite_repositories.h"
#include "domain/password_derivation.h"
#include "sqlite_test_util.h"

using namespace hlm;

class AuthServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void firstStartRequiresAdminCreation();
    void emptyOrBlankUsernameRejected();
    void noDefaultPasswordAndNoPlaintext();
    void correctPasswordLogsIn();
    void wrongPasswordFails();
    void threeFailuresLockFor30Seconds();
    void lockoutDoesNotAffectOtherAccounts();
    void disabledUserCannotLogin();
    void failedLoginAuditedWithoutPassword();
    void unknownUserRunsDummyDerivation();
    void changePasswordInvalidatesOld();
    void changePasswordDerivationFailureLeavesHashUnchanged();
    void verifyPasswordCorrect();
    void verifyPasswordIncorrect();
    void verifyPasswordUnknownUser();
    void verifyPasswordDoesNotTriggerLockout();
};

namespace {

QByteArray failingSalt()
{
    return QByteArray();
}

QByteArray failingDerive(const QString &, const QByteArray &, int)
{
    return QByteArray();
}

// Records the number of derive calls and the last iteration count (used by
// unknownUserRunsDummyDerivation to prove the dummy derivation runs).
int g_deriveCalls = 0;
int g_lastIterations = 0;

QByteArray countingDerive(const QString &, const QByteArray &, int iterations)
{
    ++g_deriveCalls;
    g_lastIterations = iterations;
    return QByteArray(32, '\x42');
}

} // namespace

void AuthServiceTest::initTestCase()
{
    QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
             "QSQLITE driver not available");
}

void AuthServiceTest::cleanup()
{
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::firstStartRequiresAdminCreation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);

        QVERIFY(auth.needsInitialAdmin());
        // No hardcoded default password: creating with an empty password fails.
        QString error;
        QVERIFY(!auth.createInitialAdmin(QStringLiteral("admin"), QString(), &error));
        QVERIFY(auth.needsInitialAdmin());

        QVERIFY2(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("s3cret!"), &error),
                 qPrintable(error));
        QVERIFY(!auth.needsInitialAdmin());
        // A second admin cannot be created while users exist.
        QVERIFY(!auth.createInitialAdmin(QStringLiteral("root"), QStringLiteral("x"), &error));
        QCOMPARE(users.countUsers(), qint64(1));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::emptyOrBlankUsernameRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);

        QString error;
        QVERIFY(!auth.createInitialAdmin(QString(), QStringLiteral("s3cret!"), &error));
        QVERIFY(!auth.createInitialAdmin(QStringLiteral("   "), QStringLiteral("s3cret!"), &error));
        QVERIFY(auth.needsInitialAdmin());
        QCOMPARE(users.countUsers(), qint64(0));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::noDefaultPasswordAndNoPlaintext()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const auto u = users.findByName(QStringLiteral("admin"));
        QVERIFY(u.has_value());
        QVERIFY(u->passwordHash != QStringLiteral("hunter2"));
        QVERIFY(!u->passwordHash.contains(QStringLiteral("hunter2")));
        QCOMPARE(u->salt.size(), kSaltBytes);
        QCOMPARE(u->iterations, kDefaultPbkdf2Iterations);
        QCOMPARE(u->role, Role::Admin);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::correctPasswordLogsIn()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const LoginResult r = auth.login(QStringLiteral("admin"), QStringLiteral("hunter2"));
        QVERIFY2(r.ok, qPrintable(r.reason));
        QVERIFY(r.user.has_value());
        QCOMPARE(r.user->username, QStringLiteral("admin"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::wrongPasswordFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const LoginResult r = auth.login(QStringLiteral("admin"), QStringLiteral("wrong"));
        QVERIFY(!r.ok);
        QCOMPARE(r.reason, QStringLiteral("bad credentials"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::threeFailuresLockFor30Seconds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        QCOMPARE(auth.login(QStringLiteral("admin"), QStringLiteral("bad1")).reason,
                 QStringLiteral("bad credentials"));
        QCOMPARE(auth.login(QStringLiteral("admin"), QStringLiteral("bad2")).reason,
                 QStringLiteral("bad credentials"));
        // Third failure locks the account (spec §11.5).
        QCOMPARE(auth.login(QStringLiteral("admin"), QStringLiteral("bad3")).reason,
                 QStringLiteral("locked"));
        // Even the correct password is rejected while locked.
        QCOMPARE(auth.login(QStringLiteral("admin"), QStringLiteral("hunter2")).reason,
                 QStringLiteral("locked"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::lockoutDoesNotAffectOtherAccounts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        UserRecord op;
        op.username = QStringLiteral("operator1");
        op.role = Role::Operator;
        op.salt = generateSalt();
        op.iterations = 1000;
        op.passwordHash = passwordHashHex(derivePasswordKey(QStringLiteral("op-pass"),
                                                            op.salt, op.iterations));
        QVERIFY(users.createUser(op, &error));

        // Lock the admin account.
        auth.login(QStringLiteral("admin"), QStringLiteral("bad1"));
        auth.login(QStringLiteral("admin"), QStringLiteral("bad2"));
        auth.login(QStringLiteral("admin"), QStringLiteral("bad3"));
        QCOMPARE(auth.login(QStringLiteral("admin"), QStringLiteral("hunter2")).reason,
                 QStringLiteral("locked"));

        // The operator account is unaffected (spec §11.5).
        const LoginResult r = auth.login(QStringLiteral("operator1"), QStringLiteral("op-pass"));
        QVERIFY2(r.ok, qPrintable(r.reason));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::disabledUserCannotLogin()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        UserRecord op;
        op.username = QStringLiteral("operator1");
        op.role = Role::Operator;
        op.salt = generateSalt();
        op.iterations = 1000;
        op.passwordHash = passwordHashHex(derivePasswordKey(QStringLiteral("op-pass"),
                                                            op.salt, op.iterations));
        op.enabled = false;
        QVERIFY(users.createUser(op, &error));

        const LoginResult r = auth.login(QStringLiteral("operator1"), QStringLiteral("op-pass"));
        QVERIFY(!r.ok);
        QCOMPARE(r.reason, QStringLiteral("disabled"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::failedLoginAuditedWithoutPassword()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        auth.login(QStringLiteral("admin"), QStringLiteral("sup3r-secret-password"));

        const QVector<AuditRecord> records = audit.recent(10);
        QVERIFY(!records.isEmpty());
        bool found = false;
        for (const AuditRecord &a : records) {
            if (a.action == QStringLiteral("auth.login") && a.result == AuditResult::Failure) {
                found = true;
                // The password must never appear in the audit log (spec §11.5, §17).
                QVERIFY(!a.redactedParameters.contains(QStringLiteral("sup3r-secret-password")));
                QVERIFY(!a.reason.contains(QStringLiteral("sup3r-secret-password")));
            }
        }
        QVERIFY(found);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

// The unknown-user path must run a dummy PBKDF2 derivation with the same
// iteration count as a real verification, so account existence is not
// observable through login timing (timing side-channel).
void AuthServiceTest::unknownUserRunsDummyDerivation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        QString error;
        AuthService setup(&users, &audit);
        QVERIFY(setup.createInitialAdmin(QStringLiteral("admin"),
                                         QStringLiteral("hunter2"), &error));

        g_deriveCalls = 0;
        g_lastIterations = 0;
        AuthService auth(&users, &audit, generateSalt, countingDerive);

        const LoginResult r = auth.login(QStringLiteral("nobody"), QStringLiteral("whatever"));
        QVERIFY(!r.ok);
        QCOMPARE(r.reason, QStringLiteral("unknown user"));
        // The dummy derivation ran with the same iteration count as a real
        // verification (kDefaultPbkdf2Iterations), keeping timing uniform.
        QCOMPARE(g_deriveCalls, 1);
        QCOMPARE(g_lastIterations, kDefaultPbkdf2Iterations);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::changePasswordInvalidatesOld()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const auto u = users.findByName(QStringLiteral("admin"));
        QVERIFY(u.has_value());
        QVERIFY2(auth.changePassword(u->id, QStringLiteral("new-pass"), &error),
                 qPrintable(error));
        QVERIFY(!auth.login(QStringLiteral("admin"), QStringLiteral("hunter2")).ok);
        QVERIFY(auth.login(QStringLiteral("admin"), QStringLiteral("new-pass")).ok);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::changePasswordDerivationFailureLeavesHashUnchanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const auto before = users.findByName(QStringLiteral("admin"));
        QVERIFY(before.has_value());

        // A failing salt generator must abort the change and leave the stored
        // hash untouched (spec §11.5: never store an empty hash).
        AuthService badSalt(&users, &audit, failingSalt, derivePasswordKey);
        QVERIFY(!badSalt.changePassword(before->id, QStringLiteral("new-pass"), &error));
        QVERIFY(!error.isEmpty());
        const auto afterSalt = users.findByName(QStringLiteral("admin"));
        QVERIFY(afterSalt.has_value());
        QCOMPARE(afterSalt->passwordHash, before->passwordHash);
        QCOMPARE(afterSalt->salt, before->salt);

        // A failing key derivation must likewise abort without corrupting.
        AuthService badDerive(&users, &audit, generateSalt, failingDerive);
        QVERIFY(!badDerive.changePassword(before->id, QStringLiteral("new-pass"), &error));
        QVERIFY(!error.isEmpty());
        const auto afterDerive = users.findByName(QStringLiteral("admin"));
        QVERIFY(afterDerive.has_value());
        QCOMPARE(afterDerive->passwordHash, before->passwordHash);
        QCOMPARE(afterDerive->salt, before->salt);

        // The original password still works after both failed attempts.
        QVERIFY(auth.login(QStringLiteral("admin"), QStringLiteral("hunter2")).ok);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::verifyPasswordCorrect()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const auto u = users.findByName(QStringLiteral("admin"));
        QVERIFY(u.has_value());
        QVERIFY(auth.verifyPassword(u->id, QStringLiteral("hunter2")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::verifyPasswordIncorrect()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const auto u = users.findByName(QStringLiteral("admin"));
        QVERIFY(u.has_value());
        QVERIFY(!auth.verifyPassword(u->id, QStringLiteral("wrong")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

void AuthServiceTest::verifyPasswordUnknownUser()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        QVERIFY(!auth.verifyPassword(9999, QStringLiteral("hunter2")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

// Pure verification must not count toward the 3-failure lockout: 3 wrong
// verifyPassword calls leave login() unlocked (spec §11.3 二次验证 vs §11.5).
void AuthServiceTest::verifyPasswordDoesNotTriggerLockout()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("auth_test"));
        QVERIFY(db.isOpen());
        SqliteUserRepository users(db);
        SqliteAuditRepository audit(db);
        AuthService auth(&users, &audit);
        QString error;
        QVERIFY(auth.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("hunter2"), &error));

        const auto u = users.findByName(QStringLiteral("admin"));
        QVERIFY(u.has_value());
        for (int i = 0; i < 3; ++i)
            QVERIFY(!auth.verifyPassword(u->id, QStringLiteral("bad")));

        // The account is NOT locked: the correct password still logs in.
        const LoginResult r = auth.login(QStringLiteral("admin"), QStringLiteral("hunter2"));
        QVERIFY2(r.ok, qPrintable(r.reason));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("auth_test"));
}

QTEST_GUILESS_MAIN(AuthServiceTest)
#include "test_auth_service.moc"
