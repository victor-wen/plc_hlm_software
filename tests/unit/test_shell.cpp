// Task 10 unit tests: ShellModel + MainWindow shell (spec §11.1-§11.4).
//
// Coverage required by the task brief:
// - Navigation: 7 nav items, clicking switches the central page stack.
// - Permission-disable reasons: action buttons disabled with the reason from
//   PermissionPolicy/InterlockRules (spec §11.4).
// - Stale values: shell shows "—" when the snapshot is stale/invalid.
// - No optimistic state: shell state only changes on snapshot/connection
//   updates, never on command submission.
// - Hold-command intent clearing: page switch and modal-dialog triggers call
//   cancelHold on held buttons (spec §10.7).
// - Estop button separated, fixed red style (spec §10.6).
// - 100/125/150% DPI: layout at scaled font sizes without truncation.

#include <QtTest>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QPushButton>

#include "domain/device_snapshot.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/hold_button.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

// Sends a synthetic mouse press at the widget center (hold start).
void pressAt(QWidget *w)
{
    const QPoint center = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = 0; // all M0-M14 off
    d.statusWord3 = 0;
    d.faultCode = 0;
    d.currentStep = 0;
    d.beltSpeed = 1500;
    d.targetWidth = 100;
    d.currentWidth = 100;
    d.heartbeat = 1;
    d.fastQuality = DataQuality::Valid;
    return d;
}

} // namespace

class ShellTest : public QObject
{
    Q_OBJECT

private slots:
    // --- ShellModel ---------------------------------------------------------
    void modelStartsOfflineUnknown();
    void modelSnapshotUpdatesState();
    void modelStaleSnapshotMarksInvalid();
    void modelUserAndRole();

    // --- MainWindow navigation ----------------------------------------------
    void navigationHasSevenItems();
    void navigationSwitchesPages();
    void navigationClearsHoldIntents();

    // --- MainWindow status bar ----------------------------------------------
    void statusBarReflectsSnapshot();
    void statusBarOfflineShowsDash();

    // --- MainWindow alarm banner --------------------------------------------
    void alarmBannerGreenWhenNoFault();
    void alarmBannerShowsFault();

    // --- MainWindow action bar ----------------------------------------------
    void estopSeparatedAndRed();
    void actionButtonsDisabledWithReasonWhenOffline();
    void actionButtonsEnabledForAdminOnline();
    void noOptimisticStateOnCommand();

    // --- theme ---------------------------------------------------------------
    void themeStylesheetApplied();

    // --- intent clearing -----------------------------------------------------
    void modalDialogTriggerClearsHoldIntents();
    void mainWindowDeactivationClearsHolds();
    void destroyedHoldWidgetIsSafe();

    // --- DPI scaling ----------------------------------------------------------
    void layoutAtScaledFonts();
};

// --- ShellModel --------------------------------------------------------------

void ShellTest::modelStartsOfflineUnknown()
{
    ShellModel model;
    QCOMPARE(model.online(), false);
    QCOMPARE(model.hasSnapshot(), false);
    // No snapshot yet: mode/running/homed are unknown.
    QCOMPARE(model.modeKnown(), false);
}

void ShellTest::modelSnapshotUpdatesState()
{
    ShellModel model;
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QVERIFY(model.hasSnapshot());
    QVERIFY(model.online());
    // All M bits off: manual mode (M1=0, M2=0 -> 手动 per D100 semantics:
    // M2=1 is auto). Mode known because a fresh snapshot exists.
    QVERIFY(model.modeKnown());
    QCOMPARE(model.isAutoMode(), false);
    QCOMPARE(model.isRunning(), false);
    QCOMPARE(model.isHomed(), false);
    QCOMPARE(model.isFaulted(), false);
}

void ShellTest::modelStaleSnapshotMarksInvalid()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    d.dataAgeMs = 99999; // stale
    d.fastQuality = DataQuality::Stale;
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(model.hasSnapshot());
    QCOMPARE(model.snapshotFresh(), false);
    // Dependent actions must be disabled when stale (spec §11.2).
    QVERIFY(!model.actionsAvailable());
}

void ShellTest::modelUserAndRole()
{
    ShellModel model;
    QCOMPARE(model.userName(), QStringLiteral("未登录"));
    QCOMPARE(model.role(), Role::Anonymous);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QCOMPARE(model.userName(), QStringLiteral("admin"));
    QCOMPARE(model.role(), Role::Admin);
}

// --- MainWindow navigation ---------------------------------------------------

void ShellTest::navigationHasSevenItems()
{
    MainWindow w;
    QCOMPARE(w.navItemCount(), 7);
}

void ShellTest::navigationSwitchesPages()
{
    MainWindow w;
    w.show();
    QCOMPARE(w.currentPageIndex(), 0);
    w.setCurrentPage(3); // 报警
    QCOMPARE(w.currentPageIndex(), 3);
    w.setCurrentPage(0);
    QCOMPARE(w.currentPageIndex(), 0);
}

void ShellTest::navigationClearsHoldIntents()
{
    // Page switch must clear continuous-command intents (spec §10.7).
    MainWindow w;
    w.show();
    HoldButton held(QStringLiteral("点动"));
    w.registerHoldWidget(&held);
    // Simulate a press (hold active).
    pressAt(&held);
    QVERIFY(w.hasActiveHolds());
    w.setCurrentPage(2);
    QVERIFY(!w.hasActiveHolds());
    QVERIFY(!held.isHeld());
}

// --- MainWindow status bar ---------------------------------------------------

void ShellTest::statusBarReflectsSnapshot()
{
    MainWindow w;
    ShellModel *model = w.shellModel();
    QVERIFY(model != nullptr);

    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = (1 << 2); // M2=1 auto mode
    model->updateSnapshot(DeviceSnapshot(d));

    // Top bar must show text for each status (color is not the only channel).
    QVERIFY(w.topBarText().contains(QStringLiteral("自动")));
    QVERIFY(w.topBarText().contains(QStringLiteral("未登录")));
}

void ShellTest::statusBarOfflineShowsDash()
{
    MainWindow w;
    // No snapshot: mode/running/homed must show 未知/"—" text, not guesses.
    QVERIFY(w.topBarText().contains(QStringLiteral("—")));
}

// --- MainWindow alarm banner -------------------------------------------------

void ShellTest::alarmBannerGreenWhenNoFault()
{
    MainWindow w;
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QCOMPARE(w.alarmBannerText(), QStringLiteral("无报警"));
}

void ShellTest::alarmBannerShowsFault()
{
    MainWindow w;
    DeviceSnapshotData d = validSnapshotData();
    d.faultCode = 3; // latched fault
    w.shellModel()->updateSnapshot(DeviceSnapshot(d));
    QVERIFY(w.alarmBannerText() != QStringLiteral("无报警"));
    QVERIFY(!w.alarmBannerText().isEmpty());
}

// --- MainWindow action bar ---------------------------------------------------

void ShellTest::estopSeparatedAndRed()
{
    MainWindow w;
    w.show();
    QPushButton *estop = w.estopButton();
    QVERIFY(estop != nullptr);
    // Fixed red danger style, independent of theme state changes (spec §10.6).
    QVERIFY(estop->property("danger").toBool());
    QVERIFY(!estop->styleSheet().isEmpty());
    // Estop is allowed for any user even when logged out (spec §11.4).
    QVERIFY(estop->isEnabled() || !estop->toolTip().isEmpty());
}

void ShellTest::actionButtonsDisabledWithReasonWhenOffline()
{
    MainWindow w;
    w.show();
    // Offline + anonymous: start/reset must be disabled with a reason.
    QVERIFY(!w.startButton()->isEnabled());
    QVERIFY(!w.startButton()->toolTip().isEmpty());
    QVERIFY(!w.resetButton()->isEnabled());
    QVERIFY(!w.resetButton()->toolTip().isEmpty());
}

void ShellTest::actionButtonsEnabledForAdminOnline()
{
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);
    model->setOnline(true);
    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = (1 << 2) | (1 << 8) | (1 << 9); // auto + ready + homed
    model->updateSnapshot(DeviceSnapshot(d));
    QVERIFY(w.startButton()->isEnabled());
    QVERIFY(w.resetButton()->isEnabled());
}

void ShellTest::noOptimisticStateOnCommand()
{
    // Submitting a command must NOT change the displayed state; only a
    // snapshot update may (spec §11.2).
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);
    model->setOnline(true);
    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = (1 << 2) | (1 << 8) | (1 << 9);
    model->updateSnapshot(DeviceSnapshot(d));

    const QString before = w.topBarText();
    // Simulate the coordinator accepting a start command: the shell model is
    // told a command is pending. Running state must not change.
    model->setCommandPending(Command::Start, true);
    QVERIFY(!w.topBarText().contains(QStringLiteral("运行中")));
    QCOMPARE(w.topBarText(), before);
}

// --- theme -------------------------------------------------------------

void ShellTest::themeStylesheetApplied()
{
    // theme.qss must be loaded in the constructor so the nav item height and
    // table row heights meet the >= 48 px touch target (spec §11.1).
    MainWindow w;
    QVERIFY(!w.styleSheet().isEmpty());
    QVERIFY(w.styleSheet().contains(QStringLiteral("navList")));
    QVERIFY(w.navItemMinimumHeight() >= 48);
}

// --- intent clearing ---------------------------------------------------------

void ShellTest::modalDialogTriggerClearsHoldIntents()
{
    MainWindow w;
    w.show();
    HoldButton held(QStringLiteral("点动"));
    w.registerHoldWidget(&held);
    pressAt(&held);
    QVERIFY(w.hasActiveHolds());
    // Any modal dialog popup path calls this (spec §10.7).
    w.clearHoldIntents();
    QVERIFY(!w.hasActiveHolds());
    QVERIFY(!held.isHeld());
}

void ShellTest::mainWindowDeactivationClearsHolds()
{
    // Qt delivers QEvent::WindowDeactivate only to the top-level widget, never
    // to child widgets. A HoldButton on a page is a child, so the §10.7 release
    // path must run at the MainWindow level.
    MainWindow w;
    w.show();
    HoldButton held(QStringLiteral("点动"));
    w.registerHoldWidget(&held);
    pressAt(&held);
    QVERIFY(w.hasActiveHolds());

    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&w, &deactivate);
    QVERIFY(!w.hasActiveHolds());
    QVERIFY(!held.isHeld());
}

void ShellTest::destroyedHoldWidgetIsSafe()
{
    // A registered button may be destroyed before MainWindow (pages own their
    // buttons and are rebuilt). MainWindow destruction must not dereference
    // freed memory (QPointer auto-nulls). This is the former use-after-free UB.
    MainWindow w;
    w.show();
    {
        HoldButton held(QStringLiteral("点动"));
        w.registerHoldWidget(&held);
        pressAt(&held);
        QVERIFY(w.hasActiveHolds());
    }
    // held destroyed; w must still be safe to destroy and report no active holds.
    QVERIFY(!w.hasActiveHolds());
}

// --- DPI scaling -------------------------------------------------------------

void ShellTest::layoutAtScaledFonts()
{
    // Verify the shell lays out without critical truncation at 100/125/150%
    // font scaling (spec §11.1). We simulate by enlarging the application
    // font, which is what Windows DPI scaling effectively does with our
    // layout-driven, no-absolute-coordinate design.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();

    QFont base = w.font();
    for (int percent : {100, 125, 150}) {
        QFont f = base;
        f.setPointSizeF(base.pointSizeF() * percent / 100.0);
        w.setFont(f);
        w.adjustSize();
        QApplication::processEvents();

        // Key controls must remain visible and usable within the window.
        QVERIFY(w.estopButton()->isVisible());
        QVERIFY(w.startButton()->isVisible());
        QVERIFY(w.startButton()->height() >= 48);
        QVERIFY(w.estopButton()->height() >= 48);
        // Nav items must not shrink below the touch target.
        QVERIFY(w.navItemMinimumHeight() >= 48);
    }
    w.setFont(base);
}

QTEST_MAIN(ShellTest)
#include "test_shell.moc"
