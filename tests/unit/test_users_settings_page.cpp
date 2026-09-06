// Task 17 unit tests: 用户与设置 page (spec §8.1, §11.3-§11.5).
//
// Coverage required by the task brief:
// - 未登录/操作员锁定: 敏感字段 (用户列表/密码/通讯配置/参数值) 不渲染, 只显示
//   锁定面板和"需要管理员登录"提示 (spec §11.4).
// - 登录失败统一提示: unknown/bad/locked/disabled 不暴露账号是否存在.
// - 会话超时: setSessionRemainingSec 倒计时提示, 提前 60 秒警告, 超时发出
//   注销 + 清零请求 (spec §11.5).
// - D122 100-20000 范围校验.
// - D204 1-32767、D220 1-15、乘积 10-200000 校验 (spec §10.3, §11.3).
// - D204 修改需再次验证管理员密码 (二次验证对话框).
// - 注销清零意图: 页面发出 clear 请求.
// - 无默认密码: 首次管理员创建流程无硬编码密码 (spec §11.5).
// - 无乐观状态: 参数写结果只来自 setParameterWriteResult (spec §11.2).

#include <QtTest>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QPointer>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QListWidget>
#include <QPushButton>

#include "domain/device_snapshot.h"
#include "ui/shell/shell_model.h"
#include "ui/pages/users_settings_model.h"
#include "ui/pages/users_settings_page.h"
#include "ui/dialogs/login_dialog.h"
#include "ui/dialogs/admin_password_dialog.h"
#include "ui/widgets/value_display.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

// Fresh snapshot with the default admin parameters: D122=1000, D204=1280,
// D220=2 (product 2560 in 10-200000).
DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.beltSpeed = 1000;   // D122
    d.pulsePerMm = 1280;  // D204
    d.widthSpeed = 2;     // D220
    d.heartbeat = 1;      // D140
    d.fastQuality = DataQuality::Valid;
    d.homeQuality = DataQuality::Valid;
    d.commandQuality = DataQuality::Valid;
    d.slowQuality = DataQuality::Valid;
    d.overallQuality = aggregateQuality(d);
    return d;
}

// Sends a synthetic click (press + release) at the widget center.
void clickAt(QWidget *w)
{
    const QPoint center = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, center,
                        w->mapToGlobal(center), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

UserRecord user(qint64 id, const QString &name, Role role)
{
    UserRecord u;
    u.id = id;
    u.username = name;
    u.role = role;
    u.enabled = true;
    return u;
}

} // namespace

class UsersSettingsPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- model: permission / lock panel (spec §11.4) -------------------------
    void anonymousAndOperatorAreLocked();
    void adminSeesSensitiveSections();

    // --- model: login lockout (spec §11.5) ------------------------------------
    void lockedResultShowsLockoutMessage();
    void badCredentialsShowsError();

    // --- model: session countdown (spec §11.5) --------------------------------
    void sessionCountdownWarningBeforeExpiry();
    void sessionExpiryEmitsLogoutAndClear();

    // --- model: D122 range (100-20000) ----------------------------------------
    void d122RangeValidation();

    // --- model: D204/D220 and product validation (spec §10.3) -----------------
    void d204D220ProductValidation();
    void parameterProductUsesSnapshotCounterpart();

    // --- model: serial config validation (spec §8.1) --------------------------
    void serialConfigValidation();
    void serialSaveDisablesUntilResult();

    // --- page: D204 re-auth dialog (spec §11.3) -------------------------------
    void d204RequiresReauthDialog();
    void d204CancelDoesNotWrite();

    // --- page: login failure visible on locked panel (spec §11.5) -------------
    void loginFailureShownOnLockedPanel();
    void loginDialogStaysOpenOnFailure();

    // --- page: session expiry re-arms after re-login (spec §11.5) -------------
    void sessionExpiryReArmsAfterPositiveTick();

    // --- page: serial config feed (spec §8.1) ---------------------------------
    void serialConfigFeedEchoesStoredValues();

    // --- page: change password UI (brief: 用户增删改密) ------------------------
    void changePasswordEmitsRequestWithUserId();
    void changePasswordRejectsMismatchAndUnknownUser();
    void accountOperationResultsAreExplicit();

    // --- page: user list row height >= 48 px (spec §11.1) ---------------------
    void userListRowHeightMeetsTouchTarget();

    // --- page: logout clear intent (spec §11.5) -------------------------------
    void logoutButtonEmitsClearRequest();

    // --- page: no default password (spec §11.5) -------------------------------
    void noDefaultPasswordForInitialAdmin();

    // --- page: no optimistic parameter write (spec §11.2) ---------------------
    void parameterWriteNeverShowsSuccessOptimistically();

    // --- page: rendering -------------------------------------------------------
    void pageShowsSnapshotParameterValues();
    void editorControlsMeetTouchTargetSize();

    // --- MainWindow integration ------------------------------------------------
    void mainWindowUsesUsersSettingsPage();
};

// --- model: permission / lock panel ---------------------------------------------

void UsersSettingsPageTest::anonymousAndOperatorAreLocked()
{
    ShellModel model;
    UsersSettingsModel m(model);

    // Anonymous (未登录): locked, no sensitive sections.
    QVERIFY(m.locked());
    QVERIFY(!m.isAdmin());

    // Operator: locked too (spec §11.4: 用户、通讯和参数设置仅管理员).
    model.setUser(QStringLiteral("operator"), Role::Operator);
    QVERIFY(m.locked());
    QVERIFY(!m.isAdmin());

    // Admin: unlocked.
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QVERIFY(!m.locked());
    QVERIFY(m.isAdmin());
}

void UsersSettingsPageTest::adminSeesSensitiveSections()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    // Admin panel visible; locked/create-admin panels hidden.
    QCOMPARE(page.currentPanel(), page.adminPanel());
    QVERIFY(!page.lockedPanel()->isVisible());
    QVERIFY(!page.createAdminPanel()->isVisible());

    // Sensitive sections present for the admin: user list, serial config,
    // parameter editors.
    QVERIFY(page.userList() != nullptr);
    QVERIFY(page.comPortEdit() != nullptr);
    QVERIFY(page.d122Spin() != nullptr);
    QVERIFY(page.d204Spin() != nullptr);
    QVERIFY(page.d220Spin() != nullptr);
}

// --- model: login lockout ---------------------------------------------------------

void UsersSettingsPageTest::lockedResultShowsLockoutMessage()
{
    ShellModel model;
    UsersSettingsModel m(model);

    LoginResult locked;
    locked.ok = false;
    locked.reason = QStringLiteral("locked");
    m.setLoginResult(locked);

    QVERIFY(m.loginLocked());
    QCOMPARE(m.loginStatusText(),
             QStringLiteral("用户名或密码错误；连续失败的账号可能暂时锁定"));

    // A later successful login clears the lockout display.
    LoginResult ok;
    ok.ok = true;
    ok.user = user(1, QStringLiteral("admin"), Role::Admin);
    m.setLoginResult(ok);
    QVERIFY(!m.loginLocked());
    QVERIFY(m.loginStatusText().isEmpty());
}

void UsersSettingsPageTest::badCredentialsShowsError()
{
    ShellModel model;
    UsersSettingsModel m(model);

    LoginResult bad;
    bad.ok = false;
    bad.reason = QStringLiteral("bad credentials");
    m.setLoginResult(bad);

    QVERIFY(!m.loginLocked());
    const QString generic =
        QStringLiteral("用户名或密码错误；连续失败的账号可能暂时锁定");
    QCOMPARE(m.loginStatusText(), generic);

    LoginResult unknown;
    unknown.ok = false;
    unknown.reason = QStringLiteral("unknown user");
    m.setLoginResult(unknown);
    QCOMPARE(m.loginStatusText(), generic);

    LoginResult disabled;
    disabled.ok = false;
    disabled.reason = QStringLiteral("disabled");
    m.setLoginResult(disabled);
    QCOMPARE(m.loginStatusText(), generic);
}

// --- model: session countdown ------------------------------------------------------

void UsersSettingsPageTest::sessionCountdownWarningBeforeExpiry()
{
    ShellModel model;
    UsersSettingsModel m(model);

    // 15 分钟默认会话: 剩余 300 秒 -> 普通倒计时, 无警告.
    m.setSessionRemainingSec(300);
    QVERIFY(!m.sessionWarningActive());
    QVERIFY(!m.sessionExpired());
    QVERIFY(m.sessionStatusText().contains(QStringLiteral("会话剩余")));

    // 提前 60 秒提示 (spec §11.5).
    m.setSessionRemainingSec(60);
    QVERIFY(m.sessionWarningActive());
    QVERIFY(m.sessionStatusText().contains(QStringLiteral("自动注销")));

    m.setSessionRemainingSec(45);
    QVERIFY(m.sessionWarningActive());
    QVERIFY(m.sessionStatusText().contains(QStringLiteral("45 秒")));
}

void UsersSettingsPageTest::sessionExpiryEmitsLogoutAndClear()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QSignalSpy logoutSpy(&page, &UsersSettingsPage::logoutRequested);
    QSignalSpy clearSpy(&page, &UsersSettingsPage::logoutClearRequested);

    // 会话超时 (spec §11.5): 页面发出注销 + M42/M106-M111 清零请求.
    page.setSessionRemainingSec(0);
    QCOMPARE(logoutSpy.count(), 1);
    QCOMPARE(clearSpy.count(), 1);

    // Repeated expiry ticks must not re-emit.
    page.setSessionRemainingSec(0);
    QCOMPARE(logoutSpy.count(), 1);
    QCOMPARE(clearSpy.count(), 1);
}

// --- model: D122 range -----------------------------------------------------------------

void UsersSettingsPageTest::d122RangeValidation()
{
    ShellModel model;
    UsersSettingsModel m(model);

    // D122 皮带速度 100-20000 Hz (需求/PLC上位机地址及要求.txt).
    m.setEditedD122(99);
    QVERIFY(!m.d122Valid());
    m.setEditedD122(100);
    QVERIFY(m.d122Valid());
    m.setEditedD122(20000);
    QVERIFY(m.d122Valid());
    m.setEditedD122(20001);
    QVERIFY(!m.d122Valid());
    QVERIFY(m.paramReasons().contains(
        QStringLiteral("D122 皮带速度需在 100-20000 Hz 之间")));
}

// --- model: D204/D220 and product validation -------------------------------------------

void UsersSettingsPageTest::d204D220ProductValidation()
{
    ShellModel model;
    UsersSettingsModel m(model);

    // D204 脉冲当量 1-32767 (有符号 MUL 范围, spec §11.3).
    m.setEditedD204(0);
    QVERIFY(!m.d204Valid());
    m.setEditedD204(1);
    QVERIFY(m.d204Valid());
    m.setEditedD204(32767);
    QVERIFY(m.d204Valid());
    m.setEditedD204(32768);
    QVERIFY(!m.d204Valid());

    // D220 调宽速度 1-15.
    m.setEditedD220(0);
    QVERIFY(!m.d220Valid());
    m.setEditedD220(1);
    QVERIFY(m.d220Valid());
    m.setEditedD220(15);
    QVERIFY(m.d220Valid());
    m.setEditedD220(16);
    QVERIFY(!m.d220Valid());

    // 乘积 10 <= D204*D220 <= 200000 (DDRVI 频率范围, spec §10.3/§11.3).
    m.setEditedD204(1);
    m.setEditedD220(1); // 1*1=1 < 10
    QVERIFY(!m.productValid());
    QVERIFY(m.paramReasons().contains(
        QStringLiteral("D204×D220 需在 10-200000 之间")));

    m.setEditedD204(1280);
    m.setEditedD220(2); // 2560
    QVERIFY(m.productValid());

    m.setEditedD204(32767);
    m.setEditedD220(15); // 491505 > 200000
    QVERIFY(!m.productValid());

    // 边界: 10 和 200000 都接受.
    m.setEditedD204(5);
    m.setEditedD220(2); // 10
    QVERIFY(m.productValid());
    m.setEditedD204(100000);
    m.setEditedD220(2); // 200000
    QVERIFY(m.productValid());
}

// --- model: serial config validation -----------------------------------------------------

void UsersSettingsPageTest::parameterProductUsesSnapshotCounterpart()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    DeviceSnapshotData d = validSnapshotData();
    d.widthSpeed = 15;
    model.updateSnapshot(DeviceSnapshot(d));

    // The editor default is D220=2, but the confirmed PLC value is 15.
    // D204=20000 would produce 300000 and must be rejected before re-auth.
    page.d204Spin()->setValue(20000);
    clickAt(page.writeD204Button());
    QVERIFY(page.findChild<AdminPasswordDialog *>() == nullptr);
    QVERIFY(page.paramStatusText().contains(QStringLiteral("D204×D220")));

    d = validSnapshotData();
    d.pulsePerMm = 32767;
    model.updateSnapshot(DeviceSnapshot(d));
    QSignalSpy writeSpy(&page, &UsersSettingsPage::writeParameterRequested);
    page.d220Spin()->setValue(15);
    clickAt(page.writeD220Button());
    QCOMPARE(writeSpy.count(), 0);
    QVERIFY(page.paramStatusText().contains(QStringLiteral("D204×D220")));
}

void UsersSettingsPageTest::serialConfigValidation()
{
    ShellModel model;
    UsersSettingsModel m(model);

    // Defaults are valid (spec §8.1: 站号 1, 波特率 9600, 停止位 1, 校验 无).
    QVERIFY(m.serialConfigValid());

    // 站号 1-247.
    SerialConfig cfg = m.serialConfig();
    cfg.station = 0;
    m.setSerialConfig(cfg);
    QVERIFY(!m.serialConfigValid());
    cfg.station = 247;
    m.setSerialConfig(cfg);
    QVERIFY(m.serialConfigValid());
    cfg.station = 248;
    m.setSerialConfig(cfg);
    QVERIFY(!m.serialConfigValid());
    QVERIFY(m.serialConfigReasons().contains(
        QStringLiteral("站号需在 1-247 之间")));

    // 波特率 9600/19200.
    cfg = m.serialConfig();
    cfg.station = 1; // restore a valid station
    cfg.baudRate = 115200;
    m.setSerialConfig(cfg);
    QVERIFY(!m.serialConfigValid());
    cfg.baudRate = 19200;
    m.setSerialConfig(cfg);
    QVERIFY(m.serialConfigValid());

    // 停止位 1/2, 校验 无/奇/偶.
    cfg = m.serialConfig();
    cfg.stopBits = 3;
    m.setSerialConfig(cfg);
    QVERIFY(!m.serialConfigValid());
    cfg.stopBits = 2;
    m.setSerialConfig(cfg);
    QVERIFY(m.serialConfigValid());
    cfg.parity = QStringLiteral("奇");
    m.setSerialConfig(cfg);
    QVERIFY(m.serialConfigValid());
    cfg.parity = QStringLiteral("X");
    m.setSerialConfig(cfg);
    QVERIFY(!m.serialConfigValid());
}

void UsersSettingsPageTest::serialSaveDisablesUntilResult()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QSignalSpy saveSpy(&page, &UsersSettingsPage::saveSerialConfigRequested);

    clickAt(page.saveSerialButton());
    QCOMPARE(saveSpy.count(), 1);
    QVERIFY(!page.saveSerialButton()->isEnabled());

    clickAt(page.saveSerialButton());
    QCOMPARE(saveSpy.count(), 1);

    page.setSerialSaveResult(true, QString());
    QVERIFY(page.saveSerialButton()->isEnabled());
}

// --- page: D204 re-auth dialog ------------------------------------------------------------

void UsersSettingsPageTest::d204RequiresReauthDialog()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &UsersSettingsPage::d204WriteRequested);

    // 修改 D204 必须先通过管理员密码二次验证 (spec §11.3).
    page.d204Spin()->setValue(2560);
    clickAt(page.writeD204Button());

    auto *dialog = page.findChild<AdminPasswordDialog *>();
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    QCOMPARE(spy.count(), 0); // nothing written before verification

    // 空密码: 不发写请求.
    clickAt(dialog->okButton());
    QCOMPARE(spy.count(), 0);

    // 输入管理员密码并确认: 写请求携带密码供 Task 20 二次验证.
    dialog->passwordEdit()->setText(QStringLiteral("admin-secret"));
    clickAt(dialog->okButton());
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(2560));
    QCOMPARE(args.at(1).toString(), QStringLiteral("admin-secret"));
}

void UsersSettingsPageTest::d204CancelDoesNotWrite()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &UsersSettingsPage::d204WriteRequested);

    page.d204Spin()->setValue(2560);
    clickAt(page.writeD204Button());
    auto *dialog = page.findChild<AdminPasswordDialog *>();
    QVERIFY(dialog != nullptr);

    // Cancelling the re-auth dialog aborts the write (spec §11.3).
    clickAt(dialog->cancelButton());
    QCOMPARE(spy.count(), 0);
}

// --- page: login failure visible on locked panel -----------------------------------------

void UsersSettingsPageTest::loginFailureShownOnLockedPanel()
{
    ShellModel model;
    UsersSettingsPage page(model);
    QApplication::processEvents();

    // 未登录: 锁定面板可见, 登录状态标签位于锁定面板上 (spec §11.5).
    QCOMPARE(page.currentPanel(), page.lockedPanel());
    QVERIFY(page.loginStatusLabel() != nullptr);
    QVERIFY(page.lockedPanel()->isAncestorOf(page.loginStatusLabel()));

    // Lockout must not make a known username distinguishable from an unknown one.
    LoginResult locked;
    locked.ok = false;
    locked.reason = QStringLiteral("locked");
    page.setLoginResult(locked);
    QApplication::processEvents();
    const QString generic =
        QStringLiteral("用户名或密码错误；连续失败的账号可能暂时锁定");
    QCOMPARE(page.loginStatusLabel()->text(), generic);

    // 密码错误提示同样可见.
    LoginResult bad;
    bad.ok = false;
    bad.reason = QStringLiteral("bad credentials");
    page.setLoginResult(bad);
    QApplication::processEvents();
    QCOMPARE(page.loginStatusLabel()->text(), generic);
}

void UsersSettingsPageTest::loginDialogStaysOpenOnFailure()
{
    ShellModel model;
    UsersSettingsPage page(model);
    QApplication::processEvents();

    // 打开登录对话框, 输入凭据.
    clickAt(page.lockedPanel()->findChild<QPushButton *>(
        QStringLiteral("usersLoginButton")));
    QPointer<LoginDialog> dialog = page.findChild<LoginDialog *>();
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->isVisible());
    dialog->usernameEdit()->setText(QStringLiteral("admin"));
    dialog->passwordEdit()->setText(QStringLiteral("wrong"));
    clickAt(dialog->okButton());

    // 失败结果: 对话框不关闭, 显示错误原因, 可重试 (spec §11.5).
    LoginResult bad;
    bad.ok = false;
    bad.reason = QStringLiteral("bad credentials");
    page.setLoginResult(bad);
    QApplication::processEvents();
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->statusLabel()->text().contains(
        QStringLiteral("用户名或密码错误")));

    // 成功结果: 对话框关闭.
    LoginResult ok;
    ok.ok = true;
    ok.user = user(1, QStringLiteral("admin"), Role::Admin);
    page.setLoginResult(ok);
    QApplication::processEvents();
    // LoginDialog uses WA_DeleteOnClose, so accepting it may destroy the
    // object during processEvents(). A guarded pointer avoids dereferencing
    // freed storage in Debug builds.
    QVERIFY(dialog.isNull() || !dialog->isVisible());
}

// --- page: session expiry re-arms after re-login ------------------------------------------

void UsersSettingsPageTest::sessionExpiryReArmsAfterPositiveTick()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QSignalSpy logoutSpy(&page, &UsersSettingsPage::logoutRequested);
    QSignalSpy clearSpy(&page, &UsersSettingsPage::logoutClearRequested);

    // 第一次超时: 注销 + 清零.
    page.setSessionRemainingSec(0);
    QCOMPARE(logoutSpy.count(), 1);
    QCOMPARE(clearSpy.count(), 1);

    // 二次登录后会话重新计时 (正值): 复位超时标记.
    page.setSessionRemainingSec(600);
    QCOMPARE(logoutSpy.count(), 1); // 不复发

    // 会话再次超时: 必须再次发出注销 + 清零 (spec §11.5).
    page.setSessionRemainingSec(0);
    QCOMPARE(logoutSpy.count(), 2);
    QCOMPARE(clearSpy.count(), 2);
}

// --- page: serial config feed --------------------------------------------------------------

void UsersSettingsPageTest::serialConfigFeedEchoesStoredValues()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    // 回显实际存储的串口配置 (Task 20 接线 DatabaseService::getSetting).
    SerialConfig cfg;
    cfg.comPort = QStringLiteral("COM3");
    cfg.station = 7;
    cfg.baudRate = 19200;
    cfg.stopBits = 2;
    cfg.parity = QStringLiteral("偶");
    cfg.timeoutMs = 500;
    cfg.readRetries = 3;
    page.setSerialConfig(cfg);

    QCOMPARE(page.comPortEdit()->text(), QStringLiteral("COM3"));
    QCOMPARE(page.stationSpin()->value(), 7);
    QCOMPARE(page.baudRateCombo()->currentData().toInt(), 19200);
    QCOMPARE(page.stopBitsCombo()->currentData().toInt(), 2);
    QCOMPARE(page.parityCombo()->currentText(), QStringLiteral("偶"));
    QCOMPARE(page.timeoutSpin()->value(), 500);
    QCOMPARE(page.readRetriesSpin()->value(), 3);
}

// --- page: change password UI ----------------------------------------------------------------

void UsersSettingsPageTest::changePasswordEmitsRequestWithUserId()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.setUsers({user(1, QStringLiteral("admin"), Role::Admin),
                   user(2, QStringLiteral("operator"), Role::Operator)});
    QSignalSpy spy(&page, &UsersSettingsPage::changePasswordRequested);

    // 两次输入不一致: 拒绝, 不发请求.
    page.changePasswordUserEdit()->setText(QStringLiteral("operator"));
    page.changePasswordNewEdit()->setText(QStringLiteral("new-secret"));
    page.changePasswordConfirmEdit()->setText(QStringLiteral("other-secret"));
    clickAt(page.changePasswordButton());
    QCOMPARE(spy.count(), 0);
    QVERIFY(page.changePasswordStatusLabel()->text().contains(
        QStringLiteral("不一致")));

    // 一致: 按用户名匹配用户, 发出改密请求 (密码不回显明文).
    page.changePasswordConfirmEdit()->setText(QStringLiteral("new-secret"));
    clickAt(page.changePasswordButton());
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toLongLong(), qint64(2));
    QCOMPARE(args.at(1).toString(), QStringLiteral("new-secret"));
    QCOMPARE(page.changePasswordNewEdit()->echoMode(), QLineEdit::Password);
}

void UsersSettingsPageTest::changePasswordRejectsMismatchAndUnknownUser()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.setUsers({user(1, QStringLiteral("admin"), Role::Admin)});
    QSignalSpy spy(&page, &UsersSettingsPage::changePasswordRequested);

    // 空用户名: 拒绝.
    clickAt(page.changePasswordButton());
    QCOMPARE(spy.count(), 0);

    // 不存在的用户: 拒绝.
    page.changePasswordUserEdit()->setText(QStringLiteral("ghost"));
    page.changePasswordNewEdit()->setText(QStringLiteral("secret"));
    page.changePasswordConfirmEdit()->setText(QStringLiteral("secret"));
    clickAt(page.changePasswordButton());
    QCOMPARE(spy.count(), 0);
    QVERIFY(page.changePasswordStatusLabel()->text().contains(
        QStringLiteral("未找到")));
}

void UsersSettingsPageTest::accountOperationResultsAreExplicit()
{
    ShellModel model;
    UsersSettingsPage page(model);

    page.setNeedsInitialAdmin(true);
    page.setInitialAdminResult(false, QStringLiteral("username already exists"));
    QVERIFY(page.createAdminStatusLabel()->text().contains(QStringLiteral("已存在")));
    QVERIFY(page.createAdminButton()->isEnabled());

    page.setInitialAdminResult(true, QString());
    page.setNeedsInitialAdmin(false);
    QVERIFY(page.loginStatusLabel()->text().contains(QStringLiteral("请使用")));

    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.setAddUserResult(false, QStringLiteral("username already exists"));
    QVERIFY(page.userStatusLabel()->text().contains(QStringLiteral("已存在")));
    page.setAddUserResult(true, QString());
    QVERIFY(page.userStatusLabel()->text().contains(QStringLiteral("创建成功")));

    page.setPasswordChangeResult(true, QString());
    QVERIFY(page.changePasswordStatusLabel()->text().contains(
        QStringLiteral("修改成功")));
}

// --- page: user list row height --------------------------------------------------------------

void UsersSettingsPageTest::userListRowHeightMeetsTouchTarget()
{
    // 用户列表行高 >= 48 px, 触摸目标达标 (spec §11.1).
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.setUsers({user(1, QStringLiteral("admin"), Role::Admin)});
    QApplication::processEvents();

    QVERIFY(page.userList()->sizeHintForRow(0) >= 48);
}

// --- page: logout clear intent --------------------------------------------------------------

void UsersSettingsPageTest::logoutButtonEmitsClearRequest()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QSignalSpy logoutSpy(&page, &UsersSettingsPage::logoutRequested);
    QSignalSpy clearSpy(&page, &UsersSettingsPage::logoutClearRequested);

    // 注销触发 M42、M106-M111 清零流程 (spec §11.5).
    clickAt(page.logoutButton());
    QCOMPARE(logoutSpy.count(), 1);
    QCOMPARE(clearSpy.count(), 1);
}

// --- page: no default password ----------------------------------------------------------------

void UsersSettingsPageTest::noDefaultPasswordForInitialAdmin()
{
    ShellModel model;
    UsersSettingsPage page(model);
    page.setNeedsInitialAdmin(true);
    QApplication::processEvents();

    // 首次启动强制创建管理员, 无默认密码 (spec §11.5): the password fields
    // start empty and the page never pre-fills a password.
    QCOMPARE(page.currentPanel(), page.createAdminPanel());
    QVERIFY(page.adminPasswordEdit()->text().isEmpty());
    QVERIFY(page.adminConfirmEdit()->text().isEmpty());

    QSignalSpy spy(&page, &UsersSettingsPage::createInitialAdminRequested);

    // Empty password: rejected, nothing emitted.
    page.adminUsernameEdit()->setText(QStringLiteral("admin"));
    clickAt(page.createAdminButton());
    QCOMPARE(spy.count(), 0);

    // Mismatched confirmation: rejected.
    page.adminPasswordEdit()->setText(QStringLiteral("secret-1"));
    page.adminConfirmEdit()->setText(QStringLiteral("secret-2"));
    clickAt(page.createAdminButton());
    QCOMPARE(spy.count(), 0);

    // Matching non-empty password: emitted with exactly what was typed.
    page.adminConfirmEdit()->setText(QStringLiteral("secret-1"));
    clickAt(page.createAdminButton());
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("admin"));
    QCOMPARE(args.at(1).toString(), QStringLiteral("secret-1"));
}

// --- page: no optimistic parameter write --------------------------------------------------------

void UsersSettingsPageTest::parameterWriteNeverShowsSuccessOptimistically()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &UsersSettingsPage::writeParameterRequested);

    page.d122Spin()->setValue(1500);
    clickAt(page.writeD122Button());
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(122)); // D122
    QCOMPARE(args.at(1).toUInt(), quint16(1500)); // 携带目标值

    // No result fed yet: the page shows waiting, never success (spec §11.2).
    QVERIFY(page.paramStatusText().contains(QStringLiteral("等待")));
    QVERIFY(!page.paramStatusText().contains(QStringLiteral("成功")));

    // Success only arrives from the confirmed result feed.
    page.setParameterWriteResult(true, QStringLiteral("写入成功"));
    QVERIFY(page.paramStatusText().contains(QStringLiteral("成功")));

    // Failure result is explicit.
    page.setParameterWriteResult(false, QStringLiteral("写入失败, 请重试"));
    QVERIFY(page.paramStatusText().contains(QStringLiteral("失败")));
}

// --- page: rendering -----------------------------------------------------------------------------

void UsersSettingsPageTest::pageShowsSnapshotParameterValues()
{
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // 参数值显示来自快照 (D122/D204/D220), 无乐观更新 (spec §11.2).
    QCOMPARE(page.paramDisplay(QStringLiteral("d122"))->text(),
             QStringLiteral("1000 Hz"));
    QCOMPARE(page.paramDisplay(QStringLiteral("d204"))->text(),
             QStringLiteral("1280 脉冲/mm"));
    QCOMPARE(page.paramDisplay(QStringLiteral("d220"))->text(),
             QStringLiteral("2 mm/s"));

    // D204 单独显示"非专业人员勿修改" (spec §11.3).
    QVERIFY(page.d204WarningLabel()->text().contains(
        QStringLiteral("非专业人员勿修改")));
}

void UsersSettingsPageTest::editorControlsMeetTouchTargetSize()
{
    // 触摸目标 >= 48 px (spec §11.1).
    ShellModel model;
    UsersSettingsPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QVERIFY(page.comPortEdit()->minimumHeight() >= 48);
    QVERIFY(page.stationSpin()->minimumHeight() >= 48);
    QVERIFY(page.d122Spin()->minimumHeight() >= 48);
    QVERIFY(page.d204Spin()->minimumHeight() >= 48);
    QVERIFY(page.d220Spin()->minimumHeight() >= 48);
    QVERIFY(page.writeD204Button()->minimumHeight() >= 48);
    QVERIFY(page.logoutButton()->minimumHeight() >= 48);
}

// --- MainWindow integration ------------------------------------------------------------------------

void UsersSettingsPageTest::mainWindowUsesUsersSettingsPage()
{
    // The 用户与设置 stub is replaced by the real UsersSettingsPage at index 6.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    UsersSettingsPage *page = w.findChild<UsersSettingsPage *>();
    QVERIFY(page != nullptr);
    w.setCurrentPage(6);
    QCOMPARE(w.currentPageIndex(), 6);

    w.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();
    QVERIFY(page->isVisible());
    QVERIFY(page->adminPanel()->isVisible());
}

QTEST_MAIN(UsersSettingsPageTest)
#include "test_users_settings_page.moc"
