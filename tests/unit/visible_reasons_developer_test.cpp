// PLC-HMI-008 developer regression tests (D5): touch-visible disabled reasons.
//
// Covers: the exact reason of a disabled PermissionButton is rendered as
// inline visible text (never tooltip-only) on the action bar, the
// manual-control page and the recipe page; the text follows permission,
// communication and interlock transitions; it stays adjacent and legible; and
// displaying the reason never makes a disabled control clickable. The
// independent black-box tests own the contract-level wording-free checks;
// these tests lock the same behavior through the page APIs a developer
// changes, and they fail if the visible presentation regresses to tooltips.

#include <QtTest>

#include <QApplication>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QSignalSpy>
#include <QString>

#include "domain/device_snapshot.h"
#include "ui/MainWindow.h"
#include "ui/pages/manual_control_page.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

// Manual (M1), homed (M9), not running, no estop/fault/homing, D204/D220 valid.
DeviceSnapshotData readyManualData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;   // D128
    d.currentWidth = 150;  // D130
    d.widthDelta = 50;     // D210
    d.pulsePerMm = 1280;   // D204
    d.widthSpeed = 15;     // D220
    d.heartbeat = 1;       // D140
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

DeviceSnapshotData notHomedData()
{
    DeviceSnapshotData d = readyManualData();
    d.statusWord1 = quint16(1) << 1; // M1 manual, M9 not homed
    d.overall_quality = aggregateQuality(d);
    return d;
}

// User decision 2026-09-21: homing completion is no longer a manual gate, so
// the interlock cause used by the reason-surface case below is a latched fault
// (M14), which still blocks every manual command.
DeviceSnapshotData latchedFaultData()
{
    DeviceSnapshotData d = readyManualData();
    d.statusWord1 |= quint16(1) << 14; // M14 latched fault
    d.overall_quality = aggregateQuality(d);
    return d;
}

// Frozen developer tolerances mirroring the independent test assumptions: the
// reason label must sit close to its control and stay legible at touch sizes.
constexpr int kAdjacencyPx = 300;
constexpr int kMinimumFontHeight = 12;

int centerDistance(QWidget *root, const QWidget *a, const QWidget *b)
{
    return (a->mapTo(root, a->rect().center())
            - b->mapTo(root, b->rect().center()))
        .manhattanLength();
}

// One disabled control: its declared reason must be presented as visible,
// adjacent, legible inline text (D3/OB-5).
void verifyInlineReason(QWidget *root, PermissionButton *button)
{
    QVERIFY2(!button->isEnabled(),
             qPrintable(QStringLiteral("precondition: '%1' must be disabled")
                            .arg(button->text())));
    const QString reason = button->disabledReason();
    QVERIFY2(!reason.isEmpty(),
             qPrintable(QStringLiteral("'%1' declares no reason").arg(button->text())));
    QCOMPARE(button->visibleReasonText(), reason);
    QVERIFY(button->reasonLabel() != nullptr);
    QVERIFY2(!button->reasonLabel()->isHidden(),
             qPrintable(QStringLiteral("'%1' hides its reason label").arg(button->text())));
    QVERIFY2(button->reasonLabel()->isVisibleTo(root),
             qPrintable(QStringLiteral("'%1' reason label is not visible on the page")
                            .arg(button->text())));
    QVERIFY(button->reasonLabel()->fontMetrics().height() >= kMinimumFontHeight);
    QVERIFY2(centerDistance(root, button, button->reasonLabel()) <= kAdjacencyPx,
             qPrintable(QStringLiteral("'%1' reason is not adjacent").arg(button->text())));
}

// A disabled width jog (HoldButton, no PermissionButton reason API): its
// interlock reason must still be presented as visible, adjacent, legible inline
// text on the page (same surface requirement as verifyInlineReason).
void verifyInlineHoldReason(QWidget *root, HoldButton *button, QLabel *label)
{
    QVERIFY2(!button->isEnabled(),
             qPrintable(QStringLiteral("precondition: '%1' must be disabled")
                            .arg(button->text())));
    QVERIFY2(label != nullptr, "width jog has no reason label");
    const QString reason = label->text();
    QVERIFY2(!reason.isEmpty(),
             qPrintable(QStringLiteral("'%1' declares no reason").arg(button->text())));
    QCOMPARE(button->toolTip(), reason);
    QVERIFY2(!label->isHidden(),
             qPrintable(QStringLiteral("'%1' hides its reason label").arg(button->text())));
    QVERIFY2(label->isVisibleTo(root),
             qPrintable(QStringLiteral("'%1' reason label is not visible on the page")
                            .arg(button->text())));
    QVERIFY(label->fontMetrics().height() >= kMinimumFontHeight);
    QVERIFY2(centerDistance(root, button, label) <= kAdjacencyPx,
             qPrintable(QStringLiteral("'%1' reason is not adjacent").arg(button->text())));
}

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

} // namespace

class VisibleReasonsDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void permissionButtonShowsReasonInlineAndClearsOnAllow();
    void actionBarShowsInlineReasonForEveryDisabledControl();
    void manualPageReasonFollowsGateAndDistinguishesCauses();
    void recipeApplyReasonTracksRoleTransitions();
    void inlineReasonDoesNotMakeDisabledRecipeControlClickable();
    void actionBarKeepsSafetyControlsUsableAtCompactSize();
};

void VisibleReasonsDeveloperTest::permissionButtonShowsReasonInlineAndClearsOnAllow()
{
    PermissionButton button;
    const QString reason = QStringLiteral("需要管理员权限");
    button.setEnabledWithReason(false, reason);
    QVERIFY(!button.isEnabled());
    QCOMPARE(button.disabledReason(), reason);
    QCOMPARE(button.visibleReasonText(), reason);
    QVERIFY(button.reasonLabel() != nullptr);
    QVERIFY(!button.reasonLabel()->isHidden());
    QVERIFY(button.reasonLabel()->fontMetrics().height() >= kMinimumFontHeight);
    // Tooltip/status tip remain supplements, never the only explanation.
    QCOMPARE(button.toolTip(), reason);
    QCOMPARE(button.statusTip(), reason);

    // A changed reason updates the visible text on every state change.
    button.setEnabledWithReason(false, QStringLiteral("未回原点"));
    QCOMPARE(button.visibleReasonText(), QStringLiteral("未回原点"));

    // Allowed: enabled with no stale reason left behind.
    button.setEnabledWithReason(true, QString());
    QVERIFY(button.isEnabled());
    QVERIFY(button.visibleReasonText().isEmpty());
    QVERIFY(button.reasonLabel()->isHidden());
}

void VisibleReasonsDeveloperTest::actionBarShowsInlineReasonForEveryDisabledControl()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();
    w.shellModel()->setUser(QString(), Role::Anonymous);
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    const QVector<PermissionButton *> controls{
        bar->manualButton(), bar->autoButton(), bar->startButton(),
        bar->stopButton(),    bar->resetButton(), bar->estopButton()};
    int checked = 0;
    for (PermissionButton *control : controls) {
        if (control->isEnabled() || control->disabledReason().isEmpty())
            continue;
        verifyInlineReason(bar, control);
        ++checked;
    }
    QVERIFY2(checked > 0,
             "precondition: an anonymous offline session disables action controls");
}

void VisibleReasonsDeveloperTest::manualPageReasonFollowsGateAndDistinguishesCauses()
{
    // Permission cause: anonymous with otherwise ready data.
    ShellModel permissionModel;
    ManualControlPage permissionPage(permissionModel);
    permissionModel.setUser(QString(), Role::Anonymous);
    permissionModel.updateSnapshot(DeviceSnapshot(readyManualData()));
    permissionPage.resize(1280, 720);
    permissionPage.show();
    QApplication::processEvents();
    QVERIFY(!permissionPage.stopGateButton()->isEnabled());
    verifyInlineReason(&permissionPage, permissionPage.stopGateButton());
    const QString permissionText =
        permissionPage.stopGateButton()->visibleReasonText();
    QVERIFY(permissionText.contains(QStringLiteral("管理员")));

    // Interlock cause: administrator with connected data that is not homed.
    // User decision 2026-09-21: the width jogs no longer require homing, so the
    // interlock cause is a latched fault (M14). The width jogs stay disabled and
    // carry their visible reason inline beneath their controls; the belt jog and
    // stop gate share the same common gates and are disabled too.
    ShellModel interlockModel;
    ManualControlPage interlockPage(interlockModel);
    interlockModel.setUser(QStringLiteral("admin"), Role::Admin);
    interlockModel.updateSnapshot(DeviceSnapshot(latchedFaultData()));
    interlockPage.resize(1280, 720);
    interlockPage.show();
    QApplication::processEvents();
    QVERIFY(!interlockPage.stopGateButton()->isEnabled());
    verifyInlineHoldReason(&interlockPage, interlockPage.widthFwdButton(),
                           interlockPage.widthReasonLabel());
    const QString interlockText = interlockPage.widthReasonLabel()->text();
    QVERIFY(interlockText.contains(QStringLiteral("锁存故障")));
    QVERIFY(interlockText != permissionText);

    // Communication cause: administrator without a snapshot.
    ShellModel commModel;
    ManualControlPage commPage(commModel);
    commModel.setUser(QStringLiteral("admin"), Role::Admin);
    commPage.resize(1280, 720);
    commPage.show();
    QApplication::processEvents();
    QVERIFY(!commPage.stopGateButton()->isEnabled());
    verifyInlineReason(&commPage, commPage.stopGateButton());
    const QString commText = commPage.stopGateButton()->visibleReasonText();
    QVERIFY(commText.contains(QStringLiteral("通讯")));
    QVERIFY(commText != permissionText);
    QVERIFY(commText != interlockText);

    // Allowed transition: the inline reason disappears with the gate.
    ShellModel readyModel;
    ManualControlPage readyPage(readyModel);
    readyModel.setUser(QStringLiteral("admin"), Role::Admin);
    readyModel.updateSnapshot(DeviceSnapshot(readyManualData()));
    readyPage.resize(1280, 720);
    readyPage.show();
    QApplication::processEvents();
    QVERIFY(readyPage.stopGateButton()->isEnabled());
    QVERIFY(readyPage.stopGateButton()->visibleReasonText().isEmpty());
    QVERIFY(readyPage.stopGateButton()->reasonLabel()->isHidden());

    // User decision 2026-09-21: an un-homed machine (M61/M9 clear) is NOT an
    // interlock cause any more — the width jogs are enabled with no reason, so
    // the gate change cannot leave a stale 未回原点 text behind.
    ShellModel notHomedModel;
    ManualControlPage notHomedPage(notHomedModel);
    notHomedModel.setUser(QStringLiteral("admin"), Role::Admin);
    notHomedModel.updateSnapshot(DeviceSnapshot(notHomedData()));
    notHomedPage.resize(1280, 720);
    notHomedPage.show();
    QApplication::processEvents();
    QVERIFY(notHomedPage.widthFwdButton()->isEnabled());
    QVERIFY(notHomedPage.widthRevButton()->isEnabled());
    QVERIFY(notHomedPage.widthReasonLabel()->text().isEmpty());
    QVERIFY(notHomedPage.widthReasonLabel()->isHidden());
}

void VisibleReasonsDeveloperTest::recipeApplyReasonTracksRoleTransitions()
{
    ShellModel model;
    RecipeWidthPage page(model);
    page.resize(1280, 720);
    page.show();
    model.setUser(QString(), Role::Anonymous);
    model.updateSnapshot(DeviceSnapshot(readyManualData()));
    QApplication::processEvents();

    QVERIFY(!page.applyButton()->isEnabled());
    verifyInlineReason(&page, page.applyButton());
    QVERIFY(page.applyButton()->visibleReasonText().contains(
        QStringLiteral("管理员")));

    // disabled -> allowed: no stale reason survives.
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QTRY_VERIFY_WITH_TIMEOUT(page.applyButton()->isEnabled(), 2000);
    QVERIFY(page.applyButton()->visibleReasonText().isEmpty());
    QVERIFY(page.applyButton()->reasonLabel()->isHidden());

    // allowed -> disabled: the reason comes back.
    model.setUser(QString(), Role::Anonymous);
    QTRY_VERIFY_WITH_TIMEOUT(!page.applyButton()->isEnabled(), 2000);
    QVERIFY(page.applyButton()->visibleReasonText().contains(
        QStringLiteral("管理员")));
    verifyInlineReason(&page, page.applyButton());
}

void VisibleReasonsDeveloperTest::inlineReasonDoesNotMakeDisabledRecipeControlClickable()
{
    ShellModel model;
    model.setUser(QString(), Role::Anonymous);
    RecipeWidthPage page(model);
    page.resize(1280, 720);
    page.show();
    QApplication::processEvents();

    QSignalSpy applySpy(&page, &RecipeWidthPage::applyAdjustRequested);
    clickAt(page.applyButton());
    QCOMPARE(applySpy.count(), 0);
    QVERIFY(!page.applyButton()->isEnabled());
    verifyInlineReason(&page, page.applyButton());
}

void VisibleReasonsDeveloperTest::actionBarKeepsSafetyControlsUsableAtCompactSize()
{
    // The inline reason must not squeeze the always-visible safety controls
    // below the touch-target floor at the compact envelope (1366x768 at 100%),
    // and the reason must stay inside its control below the top-aligned title.
    // The full compact/DPI responsive matrix belongs to PLC-HMI-006; this
    // guards the regression this change could introduce.
    MainWindow w;
    w.resize(1366, 768);
    w.show();
    QApplication::processEvents();
    w.shellModel()->setUser(QString(), Role::Anonymous);
    QApplication::processEvents();

    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    // Safety controls keep the touch-target floor and stay visible.
    for (PermissionButton *control : {bar->stopButton(), bar->estopButton()}) {
        QVERIFY2(control->isVisibleTo(bar),
                 qPrintable(control->text() + QStringLiteral(" must stay visible")));
        QVERIFY2(control->height() >= 48,
                 qPrintable(QStringLiteral("%1 dropped below the 48 px touch "
                                           "target at 1366x768: height %2")
                                .arg(control->text())
                                .arg(control->height())));
    }

    // Reasons stay legitimately rendered inside every disabled action control:
    // visible, legible, within the control bounds and never over its title.
    int checked = 0;
    for (PermissionButton *control :
         {bar->manualButton(), bar->autoButton(), bar->startButton(),
          bar->stopButton(), bar->resetButton(), bar->estopButton()}) {
        QLabel *label = control->reasonLabel();
        QVERIFY(label != nullptr);
        if (control->isEnabled() || control->disabledReason().isEmpty())
            continue;
        QVERIFY2(!label->isHidden(),
                 qPrintable(control->text() + QStringLiteral(" must show its reason")));
        QVERIFY(label->fontMetrics().height() >= kMinimumFontHeight);
        const int labelTop = label->geometry().top();
        const int labelBottom = label->geometry().bottom();
        const int titleBottom = control->fontMetrics().height();
        QVERIFY2(labelBottom <= control->height() - 2,
                 qPrintable(QStringLiteral("%1 reason exceeds its control: bottom %2, "
                                           "height %3")
                                .arg(control->text())
                                .arg(labelBottom)
                                .arg(control->height())));
        QVERIFY2(labelTop >= titleBottom,
                 qPrintable(QStringLiteral("%1 reason overlaps the title: label top %2 < %3")
                                .arg(control->text())
                                .arg(labelTop)
                                .arg(titleBottom)));
        ++checked;
    }
    QVERIFY(checked > 0);
}

QTEST_MAIN(VisibleReasonsDeveloperTest)
#include "visible_reasons_developer_test.moc"
