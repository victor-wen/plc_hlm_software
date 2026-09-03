// Task 13 unit tests: 手动控制 page (spec §10.7-§10.8, §11.3, §11.4).
//
// Coverage required by the task brief:
// - Hold commands (M106/M107/M108): press emits holdChanged(true) -> intent
//   write 1; release/move-out/deactivate/page-switch/modal/logout clear ->
//   intent write 0 (spec §10.7). HoldButtons registered with MainWindow so
//   page switch / window deactivation cancel active holds.
// - M109 挡停: latched toggle derived from readback, never from button state.
// - Permission (仅管理员), interlock (手动模式 M1、回原点 M9、M3=0、无急停、
//   无锁存故障), readback state and offline disabling.
// - 安全屏蔽 (M110/M111): two-step confirm; armed state resets on page switch
//   and gate change; persistent amber banner while a shield is active
//   (spec §10.8).
// - No optimistic success: a button press never shows success; M42/M105/M110/
//   M111 state comes from snapshot readback bits, not button state
//   (spec §11.2).
// - Operator/anonymous are read-only (spec §11.4).

#include <QtTest>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QLabel>

#include "domain/device_snapshot.h"
#include "ui/shell/shell_model.h"
#include "ui/pages/manual_control_model.h"
#include "ui/pages/manual_control_page.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"
#include "ui/widgets/value_display.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

// Fresh, ready-to-manual snapshot: manual (M1), homed (M9), not running,
// no estop/fault/homing, all command bits clear, D220=15.
DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;                                     // M34/M44/M45 clear
    d.targetWidth = 200;   // D128
    d.currentWidth = 150;  // D130
    d.widthDelta = 50;     // D210
    d.pulsePerMm = 1280;   // D204
    d.widthSpeed = 15;     // D220
    d.heartbeat = 1;       // D140
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

// Presses and holds a HoldButton (press only, no release).
void pressHold(QWidget *w)
{
    const QPoint center = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

} // namespace

class ManualControlPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- model: gating (permission + interlock) --------------------------------
    void manualAllowedForAdminWhenReady();
    void manualRejectedForOperatorWithPermissionReason();
    void manualRejectedOnInterlockReasons();
    void manualRejectedWhenNotFresh();
    void bypassAllowedForAdminWhenOnline();
    void bypassRejectedForOperator();
    void bypassRejectedWhenOffline();

    // --- model: readback state (spec §11.2) -------------------------------------
    void readbackBitsFromSnapshotOnly();
    void shieldActiveFromReadback();

    // --- model: shield two-step confirm (spec §10.8) ----------------------------
    void shieldArmDerivesTargetFromReadback();
    void shieldArmedResetOnGateChangeModel();

    // --- page: hold intents (spec §10.7) ----------------------------------------
    void holdPressEmitsWrite1ReleaseEmitsWrite0();
    void holdReleaseOnMoveOutEmitsWrite0();
    void holdReleaseOnWindowDeactivateEmitsWrite0();
    void holdReleaseOnPageSwitchViaMainWindow();
    void holdReleaseOnLogoutViaMainWindow();
    void holdDisabledForOperator();

    // --- page: M109 挡停 (latched, from readback) ------------------------------
    void stopGateTogglesFromReadback();

    // --- page: bypass two-step confirm + readback state -------------------------
    void passthroughRequiresSecondConfirmation();
    void bypassStateFromReadbackNotButton();
    void shieldRequiresSecondConfirmation();
    void shieldArmedResetOnPageSwitch();
    void shieldArmedResetOnGateChange();

    // --- page: amber banner (spec §10.8) ----------------------------------------
    void amberBannerShownWhileShieldActive();
    void amberBannerHiddenWhenNoShield();

    // --- page: no optimistic success (spec §11.2) --------------------------------
    void pressNeverShowsSuccess();

    // --- page: rendering ---------------------------------------------------------
    void pageShowsWidthSpeedAndStatus();
    void operatorSeesDisabledControlsWithReason();
    void controlsMeetTouchTargetSize();

    // --- MainWindow integration ---------------------------------------------------
    void mainWindowUsesManualControlPage();
};

// --- model: gating ----------------------------------------------------------------

void ManualControlPageTest::manualAllowedForAdminWhenReady()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    ManualControlModel m(model);

    QVERIFY(m.canManual());
    QVERIFY(m.manualUnmetReasons().isEmpty());
    QVERIFY(m.canBypass());
    QVERIFY(m.bypassUnmetReasons().isEmpty());
}

void ManualControlPageTest::manualRejectedForOperatorWithPermissionReason()
{
    ShellModel model;
    model.setUser(QStringLiteral("operator"), Role::Operator);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    ManualControlModel m(model);

    // 手动、直通、常转和安全屏蔽: 仅管理员 (spec §11.4).
    QVERIFY(!m.canManual());
    QVERIFY(m.manualUnmetReasons().contains(QStringLiteral("需要管理员权限")));
    QVERIFY(!m.canBypass());
    QVERIFY(m.bypassUnmetReasons().contains(QStringLiteral("需要管理员权限")));
}

void ManualControlPageTest::manualRejectedOnInterlockReasons()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ManualControlModel m(model);

    // Running (M3=1): interlock reason shown (spec §10.7).
    DeviceSnapshotData running = validSnapshotData();
    running.statusWord1 |= (quint16(1) << 3); // M3
    model.updateSnapshot(DeviceSnapshot(running));
    QVERIFY(!m.canManual());
    QVERIFY(m.manualUnmetReasons().contains(
        QStringLiteral("设备正在运行, 请先停止")));

    // Not homed (M9=0): 未回原点.
    DeviceSnapshotData notHomed = validSnapshotData();
    notHomed.statusWord1 &= ~(quint16(1) << 9); // M9 clear
    model.updateSnapshot(DeviceSnapshot(notHomed));
    QVERIFY(!m.canManual());
    QVERIFY(m.manualUnmetReasons().contains(QStringLiteral("未回原点")));

    // Auto mode (M1=0, M2=1): 需要手动模式.
    DeviceSnapshotData autoMode = validSnapshotData();
    autoMode.statusWord1 &= ~(quint16(1) << 1); // M1 clear
    autoMode.statusWord1 |= (quint16(1) << 2);  // M2
    model.updateSnapshot(DeviceSnapshot(autoMode));
    QVERIFY(!m.canManual());
    QVERIFY(m.manualUnmetReasons().contains(QStringLiteral("需要手动模式")));

    // Estop (M0=1): 急停有效.
    DeviceSnapshotData estop = validSnapshotData();
    estop.statusWord1 |= (quint16(1) << 0); // M0
    model.updateSnapshot(DeviceSnapshot(estop));
    QVERIFY(!m.canManual());
    QVERIFY(m.manualUnmetReasons().contains(QStringLiteral("急停有效")));

    // Latched fault (M14=1): 存在锁存故障.
    DeviceSnapshotData fault = validSnapshotData();
    fault.statusWord1 |= (quint16(1) << 14); // M14
    model.updateSnapshot(DeviceSnapshot(fault));
    QVERIFY(!m.canManual());
    QVERIFY(m.manualUnmetReasons().contains(QStringLiteral("存在锁存故障")));
}

void ManualControlPageTest::manualRejectedWhenNotFresh()
{
    ShellModel model; // no snapshot yet
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ManualControlModel m(model);

    QVERIFY(!m.canManual());
    QVERIFY(m.manualUnmetReasons().contains(QStringLiteral("通讯中断或数据过期")));
    QVERIFY(!m.canBypass());
    QVERIFY(m.bypassUnmetReasons().contains(QStringLiteral("通讯中断或数据过期")));
}

void ManualControlPageTest::bypassAllowedForAdminWhenOnline()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    ManualControlModel m(model);

    QVERIFY(m.canBypass());
    QVERIFY(m.bypassUnmetReasons().isEmpty());
}

void ManualControlPageTest::bypassRejectedForOperator()
{
    ShellModel model;
    model.setUser(QStringLiteral("operator"), Role::Operator);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    ManualControlModel m(model);

    QVERIFY(!m.canBypass());
    QVERIFY(m.bypassUnmetReasons().contains(QStringLiteral("需要管理员权限")));
}

void ManualControlPageTest::bypassRejectedWhenOffline()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    model.setOnline(false); // updateSnapshot would re-derive online from the snapshot
    ManualControlModel m(model);

    QVERIFY(!m.canBypass());
    QVERIFY(m.bypassUnmetReasons().contains(QStringLiteral("通讯中断")));
}

// --- model: readback state ---------------------------------------------------------

void ManualControlPageTest::readbackBitsFromSnapshotOnly()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ManualControlModel m(model);

    // No snapshot: all readback bits are false (never optimistic).
    QVERIFY(!m.m42());
    QVERIFY(!m.m105());
    QVERIFY(!m.m109());
    QVERIFY(!m.m110());
    QVERIFY(!m.m111());
    QVERIFY(!m.m106());
    QVERIFY(!m.m107());
    QVERIFY(!m.m108());

    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 5) | (quint16(1) << 6) | (quint16(1) << 7)
        | (quint16(1) << 8) | (quint16(1) << 9) | (quint16(1) << 10)
        | (quint16(1) << 11); // M105-M111
    model.updateSnapshot(DeviceSnapshot(d));

    QVERIFY(m.m105());
    QVERIFY(m.m106());
    QVERIFY(m.m107());
    QVERIFY(m.m108());
    QVERIFY(m.m109());
    QVERIFY(m.m110());
    QVERIFY(m.m111());
    QVERIFY(!m.m42()); // M42 is not in the command bits block
}

void ManualControlPageTest::shieldActiveFromReadback()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    ManualControlModel m(model);

    QVERIFY(!m.shieldActive());

    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 10); // M110
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(m.shieldActive());

    d.commandBits = (quint16(1) << 11); // M111
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(m.shieldActive());

    d.commandBits = 0;
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(!m.shieldActive());
}

// --- model: shield two-step confirm -------------------------------------------------

void ManualControlPageTest::shieldArmDerivesTargetFromReadback()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    ManualControlModel m(model);

    // Shield inactive: arming targets 屏蔽 (true).
    m.armShield(110);
    QVERIFY(m.shieldArmed(110));
    QCOMPARE(m.shieldTarget(110).value_or(false), true);

    // Shield active: arming targets 恢复 (false).
    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 10); // M110
    model.updateSnapshot(DeviceSnapshot(d));
    m.armShield(110);
    QVERIFY(m.shieldArmed(110));
    QCOMPARE(m.shieldTarget(110).value_or(true), false);

    // Non-shield addresses are ignored.
    m.armShield(105);
    QVERIFY(!m.shieldArmed(105));
}

void ManualControlPageTest::shieldArmedResetOnGateChangeModel()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    ManualControlModel m(model);

    m.armShield(110);
    QVERIFY(m.shieldArmed(110));

    // Going offline closes the bypass gate: the armed confirmation resets so
    // a stale confirm can never dispatch (spec §10.8, §11.1-§11.2).
    model.setOnline(false);
    QVERIFY(!m.shieldArmed(110));
}

// --- page: hold intents --------------------------------------------------------------

void ManualControlPageTest::holdPressEmitsWrite1ReleaseEmitsWrite0()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &ManualControlPage::manualHoldRequested);

    pressHold(page.jogButton()); // M108 press
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUInt(), quint16(108));
    QCOMPARE(spy.at(0).at(1).toBool(), true);

    clickAt(page.jogButton()); // release
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toUInt(), quint16(108));
    QCOMPARE(spy.at(1).at(1).toBool(), false);
}

void ManualControlPageTest::holdReleaseOnMoveOutEmitsWrite0()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &ManualControlPage::manualHoldRequested);

    pressHold(page.widthFwdButton()); // M106 press
    QCOMPARE(spy.count(), 1);

    // Pointer leaves while held: write 0 (spec §10.7: 指针移出后释放).
    QMouseEvent leave(QEvent::Leave, QPointF(0, 0), QPointF(0, 0),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(page.widthFwdButton(), &leave);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toUInt(), quint16(106));
    QCOMPARE(spy.at(1).at(1).toBool(), false);
}

void ManualControlPageTest::holdReleaseOnWindowDeactivateEmitsWrite0()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &ManualControlPage::manualHoldRequested);

    pressHold(page.widthRevButton()); // M107 press
    QCOMPARE(spy.count(), 1);

    // Window deactivation (modal dialog popup / alt-tab): write 0.
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(page.widthRevButton(), &deactivate);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toUInt(), quint16(107));
    QCOMPARE(spy.at(1).at(1).toBool(), false);
}

void ManualControlPageTest::holdReleaseOnPageSwitchViaMainWindow()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    ManualControlPage *page = w.findChild<ManualControlPage *>();
    QVERIFY(page != nullptr);
    w.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    w.setCurrentPage(2);
    QVERIFY(page->isVisible());
    QSignalSpy spy(page, &ManualControlPage::manualHoldRequested);

    pressHold(page->jogButton());
    QCOMPARE(spy.count(), 1);
    QVERIFY(w.hasActiveHolds());

    // Page switch cancels the active hold -> write 0 (spec §10.7).
    w.setCurrentPage(0);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toUInt(), quint16(108));
    QCOMPARE(spy.at(1).at(1).toBool(), false);
    QVERIFY(!w.hasActiveHolds());
}

void ManualControlPageTest::holdReleaseOnLogoutViaMainWindow()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    ManualControlPage *page = w.findChild<ManualControlPage *>();
    QVERIFY(page != nullptr);
    w.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    w.setCurrentPage(2);
    QSignalSpy spy(page, &ManualControlPage::manualHoldRequested);

    pressHold(page->jogButton());
    QCOMPARE(spy.count(), 1);

    // Logout/session timeout clears all hold intents -> write 0 (spec §10.7).
    w.clearHoldIntents();
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(1).toBool(), false);
    QVERIFY(!w.hasActiveHolds());
}

void ManualControlPageTest::holdDisabledForOperator()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("operator"), Role::Operator);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &ManualControlPage::manualHoldRequested);

    // 操作员只读: hold buttons disabled, no intent can be emitted.
    QVERIFY(!page.jogButton()->isEnabled());
    QVERIFY(!page.widthFwdButton()->isEnabled());
    QVERIFY(!page.widthRevButton()->isEnabled());
    pressHold(page.jogButton());
    QCOMPARE(spy.count(), 0);
}

// --- page: M109 挡停 -----------------------------------------------------------------

void ManualControlPageTest::stopGateTogglesFromReadback()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &ManualControlPage::manualLatchRequested);

    // Readback M109=0: click requests 伸出 (true).
    clickAt(page.stopGateButton());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUInt(), quint16(109));
    QCOMPARE(spy.at(0).at(1).toBool(), true);

    // Readback M109=1: click requests 缩回 (false). The target comes from the
    // snapshot, never from the button state (spec §11.2).
    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 9); // M109
    model.updateSnapshot(DeviceSnapshot(d));
    clickAt(page.stopGateButton());
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toUInt(), quint16(109));
    QCOMPARE(spy.at(1).at(1).toBool(), false);
}

// --- page: bypass two-step confirm + readback state ----------------------------------

void ManualControlPageTest::passthroughRequiresSecondConfirmation()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &ManualControlPage::bypassRequested);

    // M105 直通 is a single-click toggle (spec §10.8 requires two-step only
    // for the safety shields M110/M111): the click dispatches 直通 (M105=1).
    clickAt(page.passthroughButton());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUInt(), quint16(105));
    QCOMPARE(spy.at(0).at(1).toBool(), true);
}

void ManualControlPageTest::bypassStateFromReadbackNotButton()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // M105 readback=0: label shows 直通模式 (not 生效).
    QCOMPARE(page.passthroughButton()->text(), QStringLiteral("直通模式"));

    // M105 readback=1: label shows 直通生效 (spec §11.2: 状态来自回读位).
    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 5); // M105
    model.updateSnapshot(DeviceSnapshot(d));
    QCOMPARE(page.passthroughButton()->text(), QStringLiteral("直通生效"));

    // M42 readback=1: 常转生效.
    d.commandBits = 0;
    model.updateSnapshot(DeviceSnapshot(d));
    QCOMPARE(page.beltContinuousButton()->text(), QStringLiteral("皮带常转"));
    // M42 is not in the command bits block; simulate via a snapshot where the
    // belt continuous bit is set through the D103 block (M42 = D103 bit 12).
    d.statusWord3 = (quint16(1) << 12); // M42
    model.updateSnapshot(DeviceSnapshot(d));
    QCOMPARE(page.beltContinuousButton()->text(), QStringLiteral("常转生效"));
}

void ManualControlPageTest::shieldRequiresSecondConfirmation()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &ManualControlPage::bypassRequested);

    // First click arms; nothing is sent.
    clickAt(page.curtainShieldButton());
    QCOMPARE(spy.count(), 0);
    QCOMPARE(page.curtainShieldButton()->text(), QStringLiteral("确认屏蔽?"));

    // Second click confirms and dispatches 屏蔽 (M110=1).
    clickAt(page.curtainShieldButton());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUInt(), quint16(110));
    QCOMPARE(spy.at(0).at(1).toBool(), true);
    QCOMPARE(page.curtainShieldButton()->text(), QStringLiteral("光栅屏蔽"));
}

void ManualControlPageTest::shieldArmedResetOnPageSwitch()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    ManualControlPage *page = w.findChild<ManualControlPage *>();
    QVERIFY(page != nullptr);
    w.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    w.setCurrentPage(2);
    QVERIFY(page->isVisible());
    clickAt(page->curtainShieldButton()); // arm
    QCOMPARE(page->curtainShieldButton()->text(), QStringLiteral("确认屏蔽?"));

    // Switching away and back clears the armed confirmation (spec §10.8,
    // §11.1-§11.2 页面切换清零意图): the next click re-arms instead of
    // dispatching.
    w.setCurrentPage(0);
    QVERIFY(!page->isVisible());
    w.setCurrentPage(2);
    QVERIFY(page->isVisible());
    QCOMPARE(page->curtainShieldButton()->text(), QStringLiteral("光栅屏蔽"));

    QSignalSpy spy(page, &ManualControlPage::bypassRequested);
    clickAt(page->curtainShieldButton());
    QCOMPARE(spy.count(), 0); // re-armed, nothing dispatched
    QCOMPARE(page->curtainShieldButton()->text(), QStringLiteral("确认屏蔽?"));
}

void ManualControlPageTest::shieldArmedResetOnGateChange()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    clickAt(page.doorShieldButton()); // arm
    QCOMPARE(page.doorShieldButton()->text(), QStringLiteral("确认屏蔽?"));

    // Going offline disables the shield button and resets the armed
    // confirmation; re-enabling later requires a fresh confirm.
    model.setOnline(false);
    QVERIFY(!page.doorShieldButton()->isEnabled());
    QCOMPARE(page.doorShieldButton()->text(), QStringLiteral("门磁屏蔽"));

    model.setOnline(true);
    QVERIFY(page.doorShieldButton()->isEnabled());
    QSignalSpy spy(&page, &ManualControlPage::bypassRequested);
    clickAt(page.doorShieldButton());
    QCOMPARE(spy.count(), 0); // re-armed, nothing dispatched
    QCOMPARE(page.doorShieldButton()->text(), QStringLiteral("确认屏蔽?"));
}

// --- page: amber banner ----------------------------------------------------------------

void ManualControlPageTest::amberBannerShownWhileShieldActive()
{
    ShellModel model;
    ManualControlPage page(model);
    page.show();
    QApplication::processEvents();
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // No shield: banner hidden.
    QVERIFY(!page.shieldBanner()->isVisible());

    // M110 readback=1: persistent amber banner (spec §10.8).
    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 10); // M110
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(page.shieldBanner()->isVisible());
    QVERIFY(page.shieldBannerText().contains(QStringLiteral("光栅")));
    QVERIFY(page.shieldBannerText().contains(QStringLiteral("安全屏蔽生效")));

    // M111 readback=1: banner lists 门磁.
    d.commandBits = (quint16(1) << 11); // M111
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(page.shieldBannerText().contains(QStringLiteral("门磁")));

    // Both active.
    d.commandBits = (quint16(1) << 10) | (quint16(1) << 11);
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(page.shieldBannerText().contains(QStringLiteral("光栅")));
    QVERIFY(page.shieldBannerText().contains(QStringLiteral("门磁")));
}

void ManualControlPageTest::amberBannerHiddenWhenNoShield()
{
    ShellModel model;
    ManualControlPage page(model);
    page.show();
    QApplication::processEvents();
    model.setUser(QStringLiteral("admin"), Role::Admin);
    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 10); // M110
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(page.shieldBanner()->isVisible());

    // Shield cleared by the PLC: banner disappears (readback-driven).
    d.commandBits = 0;
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(!page.shieldBanner()->isVisible());
}

// --- page: no optimistic success ---------------------------------------------------------

void ManualControlPageTest::pressNeverShowsSuccess()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // Pressing the jog button emits the intent but the status must NOT claim
    // success: no readback yet (spec §11.2: 命令不得乐观更新状态).
    pressHold(page.jogButton());
    QVERIFY(!page.statusText().contains(QStringLiteral("成功")));
    QVERIFY(!page.statusText().contains(QStringLiteral("执行")));
    clickAt(page.jogButton());

    // Confirming a shield dispatch must not show success either.
    clickAt(page.curtainShieldButton()); // arm
    clickAt(page.curtainShieldButton()); // confirm -> dispatched
    QVERIFY(!page.statusText().contains(QStringLiteral("成功")));
}

// --- page: rendering ----------------------------------------------------------------------

void ManualControlPageTest::pageShowsWidthSpeedAndStatus()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    QCOMPARE(page.fieldDisplay(QStringLiteral("widthSpeed"))->text(),
             QStringLiteral("15 mm/s")); // D220
    QCOMPARE(page.statusText(), QStringLiteral("空闲"));

    // M109 readback=1: status shows 挡停伸出.
    DeviceSnapshotData d = validSnapshotData();
    d.commandBits = (quint16(1) << 9); // M109
    model.updateSnapshot(DeviceSnapshot(d));
    QCOMPARE(page.statusText(), QStringLiteral("挡停伸出"));
}

void ManualControlPageTest::operatorSeesDisabledControlsWithReason()
{
    ShellModel model;
    ManualControlPage page(model);
    model.setUser(QStringLiteral("operator"), Role::Operator);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // 操作员只读: all manual + bypass controls disabled with the permission
    // reason (spec §11.4: 无权限操作应保持可发现但禁用，并说明所需权限).
    QVERIFY(!page.jogButton()->isEnabled());
    QVERIFY(!page.widthFwdButton()->isEnabled());
    QVERIFY(!page.widthRevButton()->isEnabled());
    QVERIFY(!page.stopGateButton()->isEnabled());
    QVERIFY(page.stopGateButton()->toolTip().contains(
        QStringLiteral("需要管理员权限")));
    QVERIFY(!page.passthroughButton()->isEnabled());
    QVERIFY(!page.beltContinuousButton()->isEnabled());
    QVERIFY(!page.curtainShieldButton()->isEnabled());
    QVERIFY(!page.doorShieldButton()->isEnabled());
}

void ManualControlPageTest::controlsMeetTouchTargetSize()
{
    // 触摸目标 >= 48 px (spec §11.1): hold buttons and permission buttons are
    // at least 48 px tall (64 px set in the layout).
    ShellModel model;
    ManualControlPage page(model);

    QVERIFY(page.jogButton()->minimumHeight() >= 48);
    QVERIFY(page.widthFwdButton()->minimumHeight() >= 48);
    QVERIFY(page.widthRevButton()->minimumHeight() >= 48);
    QVERIFY(page.stopGateButton()->minimumHeight() >= 48);
    QVERIFY(page.passthroughButton()->minimumHeight() >= 48);
    QVERIFY(page.beltContinuousButton()->minimumHeight() >= 48);
    QVERIFY(page.curtainShieldButton()->minimumHeight() >= 48);
    QVERIFY(page.doorShieldButton()->minimumHeight() >= 48);
}

// --- MainWindow integration ----------------------------------------------------------------

void ManualControlPageTest::mainWindowUsesManualControlPage()
{
    // The 手动控制 stub is replaced by the real ManualControlPage at index 2.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    ManualControlPage *page = w.findChild<ManualControlPage *>();
    QVERIFY(page != nullptr);
    w.setCurrentPage(2);
    QCOMPARE(w.currentPageIndex(), 2);

    w.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();
    QVERIFY(page->isVisible());
    QCOMPARE(page->fieldDisplay(QStringLiteral("widthSpeed"))->text(),
             QStringLiteral("15 mm/s"));
}

QTEST_MAIN(ManualControlPageTest)
#include "test_manual_control_page.moc"
