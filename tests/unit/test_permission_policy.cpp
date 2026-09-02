// Task 7 unit tests: role-based permission matrix (spec §11.4).
// Full combination coverage: 3 roles x all commands.

#include <QtTest>

#include "application/permission_policy.h"

using namespace hlm;

class PermissionPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void anonymousCanViewAndStopAndEstopSet();
    void operatorCanStart();
    void operatorCannotAdminOnly();
    void adminCanEverything();
    void logoutClearNotGated();
};

void PermissionPolicyTest::anonymousCanViewAndStopAndEstopSet()
{
    // 未登录: 查看/在线停止/置软件急停 allowed; everything else denied.
    QVERIFY(PermissionPolicy::check(Role::Anonymous, Command::Stop).allowed);
    QVERIFY(PermissionPolicy::check(Role::Anonymous, Command::EstopSet).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::Start).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::Reset).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::AdjustWidth).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::ModeSwitch).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::EstopRelease).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::ManualCommand).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::Bypass).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Anonymous, Command::ParameterChange).allowed);
}

void PermissionPolicyTest::operatorCanStart()
{
    // 操作员: 启动 allowed (spec §10.4).
    QVERIFY(PermissionPolicy::check(Role::Operator, Command::Start).allowed);
    QVERIFY(PermissionPolicy::check(Role::Operator, Command::Stop).allowed);
    QVERIFY(PermissionPolicy::check(Role::Operator, Command::EstopSet).allowed);
}

void PermissionPolicyTest::operatorCannotAdminOnly()
{
    // 操作员: admin-only capabilities denied (spec §11.4).
    QVERIFY(!PermissionPolicy::check(Role::Operator, Command::Reset).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Operator, Command::AdjustWidth).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Operator, Command::ModeSwitch).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Operator, Command::EstopRelease).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Operator, Command::ManualCommand).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Operator, Command::Bypass).allowed);
    QVERIFY(!PermissionPolicy::check(Role::Operator, Command::ParameterChange).allowed);
}

void PermissionPolicyTest::adminCanEverything()
{
    for (int i = 0; i < int(Command::Count); ++i) {
        const Command cmd = static_cast<Command>(i);
        if (cmd == Command::LogoutClear)
            continue; // internal, not gated
        const PermissionResult r = PermissionPolicy::check(Role::Admin, cmd);
        QVERIFY2(r.allowed, qPrintable(QStringLiteral("admin denied cmd %1: %2")
                                           .arg(int(cmd)).arg(r.reason)));
    }
}

void PermissionPolicyTest::logoutClearNotGated()
{
    // 注销清 M42/M106-M111 is an internal flow, not a user capability.
    QVERIFY(PermissionPolicy::check(Role::Anonymous, Command::LogoutClear).allowed);
    QVERIFY(PermissionPolicy::check(Role::Operator, Command::LogoutClear).allowed);
    QVERIFY(PermissionPolicy::check(Role::Admin, Command::LogoutClear).allowed);
}

QTEST_GUILESS_MAIN(PermissionPolicyTest)
#include "test_permission_policy.moc"
