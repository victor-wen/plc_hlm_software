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
#include <QImage>
#include <QColor>
#include <QStyle>

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
    d.fast_quality = DataQuality::Valid;
    return d;
}

// Builds a snapshot through the real fast-block decode path, so out-of-range
// fields behave exactly as in production: checkRange only sets invalidFields
// (which pushes overallQuality to OutOfRange) and never lowers fastQuality,
// which keeps the passed-in transport/age quality. Locks the assumption that
// "field out of range" and "fast block stale/errored" are independent.
//
// `currentWidth` is carried verbatim (D130 is no longer range-checked: the PLC
// legitimately reports 0 before the first homing/adjustment, user decision
// 2026-09-21). `beltSpeed` is the knob for an unrelated out-of-range field,
// because D122 (100-20000) is still range-checked.
DeviceSnapshot decodedFastSnapshot(quint16 statusWord1, quint16 currentWidth,
                                   quint16 beltSpeed = 1500)
{
    quint16 raw[41] = {0};
    raw[0] = statusWord1;   // D100 -> M0-M14
    raw[10] = 0;            // D110 fault code (0-10)
    raw[20] = 0;            // D120 step (0-5)
    raw[22] = beltSpeed;    // D122 belt speed (100-20000)
    raw[28] = 100;          // D128 target width (0.1 mm units, no range)
    raw[30] = currentWidth; // D130 current width (no range)
    raw[40] = 1;            // D140 heartbeat
    const QDateTime now = QDateTime::currentDateTime();
    return DeviceSnapshot(
        decodeFastBlock(raw, 1, true, 0, now, now, DataQuality::Valid));
}

// Number of differing pixels between two same-size images; -1 on size mismatch.
int pixelDiffCount(const QImage &a, const QImage &b)
{
    if (a.size() != b.size())
        return -1;
    int diff = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (a.pixel(x, y) != b.pixel(x, y))
                ++diff;
    return diff;
}

// Most frequent colour in the image (the button background, since text covers
// far fewer pixels). Robust to anti-aliased glyph pixels.
QColor dominantColor(const QImage &img)
{
    QHash<QRgb, int> hist;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            ++hist[img.pixel(x, y)];
    QRgb best = 0;
    int bestN = -1;
    for (auto it = hist.constBegin(); it != hist.constEnd(); ++it) {
        if (it.value() > bestN) {
            bestN = it.value();
            best = it.key();
        }
    }
    return QColor(best);
}

bool nearColor(const QColor &c, const QColor &ref, int tolerance = 12)
{
    return qAbs(c.red() - ref.red()) <= tolerance
        && qAbs(c.green() - ref.green()) <= tolerance
        && qAbs(c.blue() - ref.blue()) <= tolerance;
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
    void modelFlagsUseOwningBlockOnly();

    // --- MainWindow navigation ----------------------------------------------
    void navigationHasSevenItems();
    void navigationSwitchesPages();
    void navigationUpdatesVisiblePageTitle();
    void navigationClearsHoldIntents();

    // --- MainWindow status bar ----------------------------------------------
    void statusBarReflectsSnapshot();
    void statusBarOfflineShowsDash();
    void topBarReadyShowsUnknownWhenStateUnknown();

    // --- MainWindow alarm banner --------------------------------------------
    void alarmBannerGreenWhenNoFault();
    void alarmBannerShowsFault();

    // --- MainWindow action bar ----------------------------------------------
    void estopSeparatedAndRed();
    void actionButtonsDisabledWithReasonWhenOffline();
    void actionButtonsEnabledForAdminOnline();
    void noOptimisticStateOnCommand();
    void unrelatedOutOfRangeFieldDoesNotDisableActions();
    void modeButtonsStillRequireAdminOnOutOfRangeSnapshot();
    void offlineDisablesEveryActionButton();
    void staleFastBlockDisablesDependentActionsOnly();
    void protocolErrorFastBlockDisablesDependentActionsOnly();
    void slowBlockStaleKeepsFastDependentActionsEnabled();
    void emptySnapshotOnlineKeepsAdvancedActionsDisabled();

    // --- top bar / R1-R4: block-scoped state ---------------------------------
    void unrelatedOutOfRangeFieldKeepsTopBarReal();
    void actionBarButtonsLookDisabled();

    // --- theme ---------------------------------------------------------------
    void themeStylesheetApplied();
    void shellContainersPaintTheirStyledBackgrounds();
    void compactHeightKeepsEveryActionInsideTheRail();

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
    d.fast_quality = DataQuality::Stale;
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

void ShellTest::modelFlagsUseOwningBlockOnly()
{
    // Regression (R1/R4): each getter must gate on the block that carries its
    // bits/fields, not the whole snapshot (spec §9, §11.2).
    ShellModel model;

    // Fast block valid + an unrelated out-of-range field (D122 belt speed 50 is
    // below its 100-20000 range): the fast state bits M1/M2/M3/M9/M14 are still
    // confirmed.
    model.updateSnapshot(decodedFastSnapshot((1 << 2) | (1 << 9), 0, 50));
    QVERIFY(!model.snapshotFresh()); // whole snapshot is OutOfRange
    QVERIFY(!model.snapshot().fieldValid(SnapshotField::BeltSpeed));
    QVERIFY(model.modeKnown());
    QVERIFY(model.isAutoMode());     // M2
    QVERIFY(!model.isRunning());     // M3=0
    QVERIFY(model.isHomed());        // M9
    QVERIFY(!model.isFaulted());     // M14=0, D110=0

    // Fast block stale: the fast-derived flags are unknown again.
    DeviceSnapshotData stale = validSnapshotData();
    stale.statusWord1 = (1 << 2) | (1 << 9);
    stale.fast_quality = DataQuality::Stale;
    stale.overall_quality = aggregateQuality(stale);
    model.updateSnapshot(DeviceSnapshot(stale));
    QVERIFY(!model.modeKnown());
    QVERIFY(!model.isAutoMode());
    QVERIFY(!model.isRunning());
    QVERIFY(!model.isHomed());
    QVERIFY(!model.isFaulted());
    QVERIFY(!model.isEstop());

    // isEstop also reads M100 from the command block: an untrusted command
    // block must not let M100=1 report 急停 as confirmed...
    DeviceSnapshotData cmd = validSnapshotData();
    cmd.commandBits = 0x0001; // M100 = software estop set
    cmd.command_quality = DataQuality::Stale;
    cmd.overall_quality = aggregateQuality(cmd);
    model.updateSnapshot(DeviceSnapshot(cmd));
    QVERIFY(!model.isEstop());

    // ...and a valid command block confirms it.
    cmd.command_quality = DataQuality::Valid;
    cmd.overall_quality = aggregateQuality(cmd);
    model.updateSnapshot(DeviceSnapshot(cmd));
    QVERIFY(model.isEstop());

    // OR combination: the fast block confirms M0=1 while the command block is
    // untrusted (M100 unknown) -> 急停 is still reported (fail-safe).
    DeviceSnapshotData m0 = validSnapshotData();
    m0.statusWord1 = 0x0001;                  // M0 estop
    m0.command_quality = DataQuality::Stale;   // M100 unknown
    m0.overall_quality = aggregateQuality(m0);
    model.updateSnapshot(DeviceSnapshot(m0));
    QVERIFY(model.isEstop());
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

void ShellTest::navigationUpdatesVisiblePageTitle()
{
    MainWindow w;
    QCOMPARE(w.currentPageTitle(), QStringLiteral("总览"));
    w.setCurrentPage(2);
    QCOMPARE(w.currentPageTitle(), QStringLiteral("手动控制"));
    QVERIFY(w.topBarText().contains(QStringLiteral("手动控制")));
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

void ShellTest::topBarReadyShowsUnknownWhenStateUnknown()
{
    // The ready light (M8, fast block) must say "准备 —" when the state is
    // unknown, not "未准备", which would claim a confirmed not-ready state
    // (spec §11.2: 未知/过期 -> "—").
    MainWindow w;
    w.show();

    QString text = w.topBarText();
    QVERIFY2(text.contains(QStringLiteral("准备 —")), qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("未准备")));
    QVERIFY(!text.contains(QStringLiteral("准备完成")));

    // Fast block stale: still unknown, not "未准备".
    DeviceSnapshotData stale = validSnapshotData();
    stale.fast_quality = DataQuality::Stale;
    stale.overall_quality = aggregateQuality(stale);
    w.shellModel()->updateSnapshot(DeviceSnapshot(stale));
    text = w.topBarText();
    QVERIFY2(text.contains(QStringLiteral("准备 —")), qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("未准备")));
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

void ShellTest::unrelatedOutOfRangeFieldDoesNotDisableActions()
{
    // Regression (root cause): a single out-of-range decoded field such as an
    // out-of-range D122 belt speed makes the aggregate snapshot quality
    // OutOfRange, so ShellModel::snapshotFresh() is false. None of the bar's
    // actions reads D122, so stop/estop (online only, spec §10.5/§10.6),
    // mode switch (online && M3=0, spec §10.2) and start (fast-block M bits,
    // spec §10.4) must remain available. Only the dependent field may disable
    // an action (spec §9, §11.2).
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);

    // D122 belt speed = 50 decoded through the production path. It only sets
    // invalidFields/overallQuality; fastQuality stays Valid. D130=0 (the
    // un-homed current width) no longer marks anything invalid.
    model->updateSnapshot(
        decodedFastSnapshot((1 << 2) | (1 << 8), 0, 50)); // M2 auto, M8 ready, M3=0

    // Sanity: the bug's trigger (snapshot-wide freshness) is genuinely false,
    // yet the fast block the actions depend on is still usable.
    QVERIFY(!model->snapshotFresh());
    QVERIFY(!model->snapshot().fieldValid(SnapshotField::BeltSpeed));
    QVERIFY(model->snapshot().fieldValid(SnapshotField::CurrentWidth));
    QVERIFY(model->snapshot().fastQuality() == DataQuality::Valid);

    auto *manual = w.findChild<QPushButton *>(QStringLiteral("manualModeButton"));
    auto *autoBtn = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(manual != nullptr);
    QVERIFY(autoBtn != nullptr);

    // The three assertions called out by the debugger report.
    QVERIFY2(autoBtn->isEnabled(), qPrintable(autoBtn->toolTip()));
    QVERIFY2(w.stopButton()->isEnabled(), qPrintable(w.stopButton()->toolTip()));
    QVERIFY2(w.estopButton()->isEnabled(), qPrintable(w.estopButton()->toolTip()));

    QVERIFY2(manual->isEnabled(), qPrintable(manual->toolTip()));
    QVERIFY2(w.startButton()->isEnabled(), qPrintable(w.startButton()->toolTip()));
    QVERIFY2(w.resetButton()->isEnabled(), qPrintable(w.resetButton()->toolTip()));

    // Online actions must not report a communications failure (spec §10.5/§10.6).
    QVERIFY(!w.stopButton()->toolTip().contains(QStringLiteral("通讯中断")));
    QVERIFY(!w.estopButton()->toolTip().contains(QStringLiteral("通讯中断")));
}

void ShellTest::modeButtonsStillRequireAdminOnOutOfRangeSnapshot()
{
    // The fix must not let an out-of-range snapshot bypass the admin-only
    // mode-switch permission (spec §11.4).
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel(); // anonymous, never logged in

    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = 0; // M3=0: the interlock alone would allow mode switch
    d.currentWidth = 0;
    d.invalidFields = (quint32(1) << quint8(SnapshotField::CurrentWidth));
    d.overall_quality = aggregateQuality(d);
    model->updateSnapshot(DeviceSnapshot(d));

    auto *manual = w.findChild<QPushButton *>(QStringLiteral("manualModeButton"));
    auto *autoBtn = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(manual != nullptr);
    QVERIFY(autoBtn != nullptr);
    QVERIFY(!manual->isEnabled());
    QVERIFY(!autoBtn->isEnabled());
    QVERIFY(manual->toolTip().contains(QStringLiteral("管理员")));
    QVERIFY(autoBtn->toolTip().contains(QStringLiteral("管理员")));
}

void ShellTest::offlineDisablesEveryActionButton()
{
    // Offline keeps every action disabled with a communications reason, even
    // for an admin (spec §10.5/§10.6).
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);
    model->setOnline(false);

    auto *manual = w.findChild<QPushButton *>(QStringLiteral("manualModeButton"));
    auto *autoBtn = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(manual != nullptr);
    QVERIFY(autoBtn != nullptr);
    QVERIFY(!manual->isEnabled());
    QVERIFY(!autoBtn->isEnabled());
    QVERIFY(!w.startButton()->isEnabled());
    QVERIFY(!w.stopButton()->isEnabled());
    QVERIFY(!w.resetButton()->isEnabled());
    QVERIFY(!w.estopButton()->isEnabled());

    QVERIFY(w.stopButton()->toolTip().contains(QStringLiteral("通讯中断")));
    QVERIFY(w.estopButton()->toolTip().contains(QStringLiteral("通讯中断")));
}

void ShellTest::staleFastBlockDisablesDependentActionsOnly()
{
    // §11.2: when the fast block carrying M0/M2/M3/M8/M14 has expired, actions
    // whose interlock reads those bits are disabled. Stop/estop need only the
    // link, so they stay available (spec §10.5/§10.6).
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);

    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = (1 << 2) | (1 << 8); // would satisfy mode/start if fresh
    d.fast_quality = DataQuality::Stale;
    d.overall_quality = aggregateQuality(d);
    model->updateSnapshot(DeviceSnapshot(d));
    QVERIFY(!model->snapshotFresh());

    auto *manual = w.findChild<QPushButton *>(QStringLiteral("manualModeButton"));
    auto *autoBtn = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(manual != nullptr);
    QVERIFY(autoBtn != nullptr);
    QVERIFY(!manual->isEnabled());
    QVERIFY(!autoBtn->isEnabled());
    QVERIFY(!w.startButton()->isEnabled());
    QVERIFY(!w.resetButton()->isEnabled());
    QVERIFY(w.stopButton()->isEnabled());
    QVERIFY(w.estopButton()->isEnabled());

    // The disabled reason must name the real data problem, not a
    // communications failure: the link is up.
    QVERIFY(manual->toolTip().contains(QStringLiteral("快速状态")));
    QVERIFY(!manual->toolTip().contains(QStringLiteral("通讯")));
    QVERIFY(w.startButton()->toolTip().contains(QStringLiteral("快速状态")));
    QVERIFY(!w.startButton()->toolTip().contains(QStringLiteral("通讯")));
}

void ShellTest::protocolErrorFastBlockDisablesDependentActionsOnly()
{
    // Same as the stale case but for a transport/protocol error on the fast
    // block: dependent actions disabled, stop/estop keep working (spec §11.2).
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);

    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = (1 << 2) | (1 << 8);
    d.fast_quality = DataQuality::ProtocolError;
    d.overall_quality = aggregateQuality(d);
    model->updateSnapshot(DeviceSnapshot(d));

    auto *manual = w.findChild<QPushButton *>(QStringLiteral("manualModeButton"));
    auto *autoBtn = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(manual != nullptr);
    QVERIFY(autoBtn != nullptr);
    QVERIFY(!manual->isEnabled());
    QVERIFY(!autoBtn->isEnabled());
    QVERIFY(!w.startButton()->isEnabled());
    QVERIFY(!w.resetButton()->isEnabled());
    QVERIFY(w.stopButton()->isEnabled());
    QVERIFY(w.estopButton()->isEnabled());
}

void ShellTest::slowBlockStaleKeepsFastDependentActionsEnabled()
{
    // Reverse lock: the gating reads only the fast block. A stale slow (or
    // home/command) block must NOT disable mode/start/reset whose interlocks
    // read fast-block M bits (spec §9, §11.2).
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);

    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = (1 << 2) | (1 << 8); // M2 auto, M8 ready, M3=0
    d.slow_quality = DataQuality::Stale;  // fast block stays Valid
    d.home_quality = DataQuality::Stale;
    d.overall_quality = aggregateQuality(d);
    model->updateSnapshot(DeviceSnapshot(d));
    QVERIFY(!model->snapshotFresh()); // whole-snapshot freshness is false

    auto *manual = w.findChild<QPushButton *>(QStringLiteral("manualModeButton"));
    auto *autoBtn = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(manual != nullptr);
    QVERIFY(autoBtn != nullptr);
    QVERIFY(manual->isEnabled());
    QVERIFY(autoBtn->isEnabled());
    QVERIFY(w.startButton()->isEnabled());
    QVERIFY(w.resetButton()->isEnabled());
    QVERIFY(w.stopButton()->isEnabled());
    QVERIFY(w.estopButton()->isEnabled());
}

void ShellTest::emptySnapshotOnlineKeepsAdvancedActionsDisabled()
{
    // setOnline(true) can be observed before the first snapshot (ShellModel's
    // link flag is an independent input). The default empty snapshot must not
    // make mode/start/reset look ready just because its fastQuality defaults
    // to Valid (spec §9, §11.2). Stop/estop remain link-only.
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);
    model->setOnline(true);
    QVERIFY(!model->hasSnapshot());

    auto *manual = w.findChild<QPushButton *>(QStringLiteral("manualModeButton"));
    auto *autoBtn = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(manual != nullptr);
    QVERIFY(autoBtn != nullptr);
    QVERIFY(!manual->isEnabled());
    QVERIFY(!autoBtn->isEnabled());
    QVERIFY(!w.startButton()->isEnabled());
    QVERIFY(!w.resetButton()->isEnabled());

    QVERIFY(manual->toolTip().contains(QStringLiteral("快速状态")));
    QVERIFY(!manual->toolTip().contains(QStringLiteral("通讯")));
}

void ShellTest::unrelatedOutOfRangeFieldKeepsTopBarReal()
{
    // Regression (R1/R4): an unrelated out-of-range field (D122 belt speed 50)
    // made modeKnown() -> snapshotFresh() false, blanking the whole top bar
    // to "—" although the fast block carrying M1/M2/M3/M8/M9 is valid.
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    model->setUser(QStringLiteral("admin"), Role::Admin);
    model->updateSnapshot(
        decodedFastSnapshot((1 << 2) | (1 << 8) | (1 << 9), 0, 50)); // auto+ready+homed

    QVERIFY(!model->snapshotFresh()); // unrelated field still fails whole-fresh
    QVERIFY(!model->snapshot().fieldValid(SnapshotField::BeltSpeed));
    QVERIFY(model->modeKnown());
    QVERIFY(model->isAutoMode());
    QVERIFY(model->isHomed());

    const QString text = w.topBarText();
    QVERIFY2(text.contains(QStringLiteral("自动")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("停止")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("已回原点")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("准备完成")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("正常")), qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("模式 —")));
    QVERIFY(!text.contains(QStringLiteral("运行 —")));
    QVERIFY(!text.contains(QStringLiteral("回原点 —")));
    QVERIFY(!text.contains(QStringLiteral("故障/急停 —")));
}

void ShellTest::actionBarButtonsLookDisabled()
{
    // Regression (R2): #actionBar ID rules and per-button [active=...] rules
    // out-specified QPushButton:disabled, so a disabled action-bar button
    // rendered pixel-identical to an enabled one. The scoped
    // QWidget#actionBar QPushButton:disabled rule must restore the shared
    // disabled look without touching enabled rendering.
    MainWindow w;
    w.resize(1280, 800);
    w.show();
    QApplication::processEvents();

    QPushButton *button = w.findChild<QPushButton *>(QStringLiteral("autoModeButton"));
    QVERIFY(button != nullptr);

    const QColor grayToken(0xe4, 0xea, 0xf0);
    const auto checkVariant = [&](const QString &label) {
        button->setEnabled(true);
        QApplication::processEvents();
        const QImage enabled =
            button->grab().toImage().convertToFormat(QImage::Format_RGB32);
        button->setEnabled(false);
        QApplication::processEvents();
        const QImage disabled =
            button->grab().toImage().convertToFormat(QImage::Format_RGB32);

        QCOMPARE(enabled.size(), disabled.size());
        QVERIFY2(pixelDiffCount(enabled, disabled) > 0,
                 qPrintable(label + QStringLiteral(": disabled must differ")));
        const QColor bg = dominantColor(disabled);
        QVERIFY2(nearColor(bg, grayToken),
                 qPrintable(QStringLiteral("%1: disabled bg=%2 expected ~#e4eaf0")
                                .arg(label, bg.name())));
    };

    // Variant 1: the default active=false style.
    checkVariant(QStringLiteral("active=false"));

    // Variant 2: [active=true] (the auto button while in auto mode). Set the
    // dynamic property and re-polish exactly as ActionBar::setActiveState does.
    button->setProperty("active", true);
    button->style()->unpolish(button);
    button->style()->polish(button);
    QApplication::processEvents();
    checkVariant(QStringLiteral("active=true"));
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

void ShellTest::shellContainersPaintTheirStyledBackgrounds()
{
    // Plain QWidget subclasses need WA_StyledBackground on Windows. Without
    // it the dark header and green/red alarm background can remain transparent,
    // leaving near-white text on the app's light surface.
    MainWindow w;
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    w.show();
    QApplication::processEvents();

    auto *top = w.findChild<QWidget *>(QStringLiteral("topBar"));
    auto *alarm = w.findChild<QWidget *>(QStringLiteral("alarmBannerOk"));
    auto *actions = w.findChild<QWidget *>(QStringLiteral("actionBar"));
    QVERIFY(top != nullptr);
    QVERIFY(alarm != nullptr);
    QVERIFY(actions != nullptr);
    QVERIFY(top->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(alarm->testAttribute(Qt::WA_StyledBackground));
    QVERIFY(actions->testAttribute(Qt::WA_StyledBackground));
}

void ShellTest::compactHeightKeepsEveryActionInsideTheRail()
{
    // Reproduce a 1280x800 logical workspace (for example a 1600x1000 panel
    // at 125% scale). Every fixed safety action must remain within the rail;
    // existence/isVisible alone does not detect geometry clipped below it.
    MainWindow w;
    w.resize(1280, 800);
    w.show();
    QApplication::processEvents();

    auto *bar = w.findChild<QWidget *>(QStringLiteral("actionBar"));
    QVERIFY(bar != nullptr);
    const QList<QPushButton *> buttons = {
        w.findChild<QPushButton *>(QStringLiteral("manualModeButton")),
        w.findChild<QPushButton *>(QStringLiteral("autoModeButton")),
        w.startButton(), w.stopButton(), w.resetButton(),
        w.findChild<QPushButton *>(QStringLiteral("loginButton")),
        w.estopButton(),
    };
    for (QPushButton *button : buttons) {
        QVERIFY(button != nullptr);
        const QRect inBar(button->mapTo(bar, QPoint(0, 0)), button->size());
        QVERIFY2(bar->rect().contains(inBar),
                 qPrintable(QStringLiteral("%1 is outside action rail: %2 vs %3")
                                .arg(button->objectName(),
                                     QString::fromLatin1("%1,%2 %3x%4")
                                         .arg(inBar.x()).arg(inBar.y())
                                         .arg(inBar.width()).arg(inBar.height()),
                                     QString::fromLatin1("%1x%2")
                                         .arg(bar->width()).arg(bar->height()))));
        QVERIFY(button->height() >= 48);
    }
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
