// Task 12 unit tests: 配方与调宽 page (spec §10.3, §11.3, §11.4).
//
// Coverage required by the task brief:
// - Selecting a recipe only loads name+width into the editor, never writes
//   the PLC (no write intent).
// - Apply requires a second confirmation (two-step inline confirm).
// - 50-400 width validation rejects out-of-range before any write.
// - Target == current: shows "当前已是目标宽度" and sends no apply command.
// - Waiting for the PLC result: M34 pending state; success only when
//   M44=1 ∧ M45=0 ∧ D130 == applied target (spec §10.3 step 7); M45=1 is
//   failure; timeout/failure results are explicit.
// - Operator/anonymous are read-only: recipe CRUD and apply disabled with a
//   permission reason (spec §11.4).
// - No optimistic success: a button press never shows success.

#include <QtTest>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QListWidget>

#include "domain/device_snapshot.h"
#include "ui/shell/shell_model.h"
#include "ui/pages/recipe_width_model.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/widgets/value_display.h"
#include "ui/widgets/permission_button.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

// Fresh, ready-to-adjust snapshot: manual (M1), homed (M9), not running,
// no estop/fault/homing, M34/M44/M45 clear, D128=200, D130=150, D204=1280,
// D220=15 (product 19200 in 10-200000).
DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;                                     // M34/M44/M45 clear
    d.targetWidth = 200;   // D128
    d.currentWidth = 150;  // D130
    d.widthDelta = 50;     // D210 = D128 - D130
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

RecipeRecord recipe(qint64 id, const QString &name, int widthMm)
{
    RecipeRecord r;
    r.id = id;
    r.name = name;
    r.targetWidthMm = widthMm;
    return r;
}

} // namespace

class RecipeWidthPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- model: recipe selection never writes the PLC ------------------------
    void selectingRecipeLoadsEditorOnly();

    // --- model: apply gating (permission + interlock + range) ----------------
    void applyRejectedOutOfRangeBeforeAnyWrite();
    void applyRejectedForOperatorWithPermissionReason();
    void applyRejectedWhenNotFresh();
    void applyRejectedOnInterlockReasons();
    void applyAllowedForAdminWhenReady();

    // --- model: target equals current (spec §10.3 step 3) ---------------------
    void targetEqualsCurrentShowsMessageWithoutCommand();

    // --- model: waiting for the PLC result (spec §10.3 steps 5-8) -------------
    void pendingStateShowsWaitingNotSuccess();
    void successOnlyWhenM44AndD130EqualsAppliedTarget();
    void failureOnM45();
    void resultDetailFromCoordinatorShown();

    // --- page: confirmation and no optimistic success -------------------------
    void applyRequiresSecondConfirmation();
    void applyClickNeverShowsSuccess();
    void pageTargetEqualsCurrentDoesNotEmitApply();
    void synchronousResultDuringApplyNotSwallowed();
    void armedStateResetOnPageSwitch();
    void armedStateResetOnGateChange();
    void setRecipesClearsStaleEditorState();
    void saveRejectsEmptyName();

    // --- page: rendering and permission gating --------------------------------
    void pageShowsStatusAndDisplays();
    void operatorSeesDisabledApplyWithReason();
    void editorControlsMeetTouchTargetSize();

    // --- MainWindow integration ------------------------------------------------
    void mainWindowUsesRecipeWidthPage();
};

// --- model: recipe selection never writes the PLC ------------------------------

void RecipeWidthPageTest::selectingRecipeLoadsEditorOnly()
{
    ShellModel model;
    RecipeWidthModel m(model);
    m.setRecipes({recipe(1, QStringLiteral("窄幅"), 180),
                  recipe(2, QStringLiteral("宽幅"), 350)});

    m.selectRecipe(recipe(1, QStringLiteral("窄幅"), 180));

    // Selection loads name + width into the editor (spec §10.3: 选择配方只把
    // 名称和目标宽度加载到界面).
    QCOMPARE(m.editedName(), QStringLiteral("窄幅"));
    QCOMPARE(m.editedWidth(), 180);
    QVERIFY(m.selectedRecipe().has_value());
    QCOMPARE(m.selectedRecipe()->id, qint64(1));

    // No write intent: no pending command, no applied target, no result.
    QVERIFY(!m.adjustPending());
    QVERIFY(!m.adjustSucceeded());
    QVERIFY(!m.adjustFailed());
    QVERIFY(!m.appliedTarget().has_value());
}

// --- model: apply gating --------------------------------------------------------

void RecipeWidthPageTest::applyRejectedOutOfRangeBeforeAnyWrite()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    RecipeWidthModel m(model);

    // Out of range (below 50): rejected before any write intent exists.
    m.setEditedWidth(10);
    QVERIFY(!m.canApply());
    QVERIFY(m.applyUnmetReasons().contains(
        QStringLiteral("目标宽度需在 50-400 mm 之间")));
    QVERIFY(!m.adjustPending());
    QVERIFY(!m.adjustSucceeded());

    // Boundaries: 50 and 400 are accepted, 401 is rejected.
    m.setEditedWidth(50);
    QVERIFY(m.canApply());
    m.setEditedWidth(400);
    QVERIFY(m.canApply());
    m.setEditedWidth(401);
    QVERIFY(!m.canApply());
    QVERIFY(m.applyUnmetReasons().contains(
        QStringLiteral("目标宽度需在 50-400 mm 之间")));
}

void RecipeWidthPageTest::applyRejectedForOperatorWithPermissionReason()
{
    ShellModel model;
    model.setUser(QStringLiteral("operator"), Role::Operator);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    RecipeWidthModel m(model);
    m.setEditedWidth(200);

    // 配方增删改、应用调宽: 仅管理员 (spec §11.4).
    QVERIFY(!m.canApply());
    QVERIFY(m.applyUnmetReasons().contains(QStringLiteral("需要管理员权限")));
    QVERIFY(!m.canEditRecipes());
}

void RecipeWidthPageTest::applyRejectedWhenNotFresh()
{
    ShellModel model; // no snapshot yet
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthModel m(model);
    m.setEditedWidth(200);

    QVERIFY(!m.canApply());
    QVERIFY(m.applyUnmetReasons().contains(QStringLiteral("通讯中断或数据过期")));
}

void RecipeWidthPageTest::applyRejectedOnInterlockReasons()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthModel m(model);
    m.setEditedWidth(200);

    // Running (M3=1): interlock reason shown.
    DeviceSnapshotData running = validSnapshotData();
    running.statusWord1 |= (quint16(1) << 3); // M3
    model.updateSnapshot(DeviceSnapshot(running));
    QVERIFY(!m.canApply());
    QVERIFY(m.applyUnmetReasons().contains(
        QStringLiteral("设备正在运行, 请先停止")));

    // D204×D220 product out of range (1×1=1 < 10): reason shown.
    DeviceSnapshotData badFreq = validSnapshotData();
    badFreq.pulsePerMm = 1; // D204
    badFreq.widthSpeed = 1; // D220
    model.updateSnapshot(DeviceSnapshot(badFreq));
    QVERIFY(!m.canApply());
    QVERIFY(m.applyUnmetReasons().contains(
        QStringLiteral("D204×D220 需在 10-200000 之间")));

    // Adjusting (M34=1): 禁止再次应用 (spec §10.3 step 6).
    DeviceSnapshotData adjusting = validSnapshotData();
    adjusting.statusWord3 = (quint16(1) << 4); // M34
    model.updateSnapshot(DeviceSnapshot(adjusting));
    QVERIFY(!m.canApply());
    QVERIFY(m.applyUnmetReasons().contains(
        QStringLiteral("正在调宽, 请等待完成")));
}

void RecipeWidthPageTest::applyAllowedForAdminWhenReady()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    RecipeWidthModel m(model);
    m.setEditedWidth(200);

    QVERIFY(m.canApply());
    QVERIFY(m.applyUnmetReasons().isEmpty());
    QVERIFY(m.canEditRecipes());
}

// --- model: target equals current (spec §10.3 step 3) ---------------------------

void RecipeWidthPageTest::targetEqualsCurrentShowsMessageWithoutCommand()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData())); // D130 = 150
    RecipeWidthModel m(model);

    // Edited target equals the current width (D130): applying is a no-op.
    m.setEditedWidth(150);
    QVERIFY(m.targetEqualsCurrent());
    QCOMPARE(m.adjustStatusText(), QStringLiteral("当前已是目标宽度"));

    // A different target is not "already at target".
    m.setEditedWidth(151);
    QVERIFY(!m.targetEqualsCurrent());

    // No write intent was ever created.
    QVERIFY(!m.adjustPending());
    QVERIFY(!m.appliedTarget().has_value());
}

// --- model: waiting for the PLC result (spec §10.3 steps 5-8) ------------------

void RecipeWidthPageTest::pendingStateShowsWaitingNotSuccess()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    RecipeWidthModel m(model);

    m.beginApply(300);
    QVERIFY(m.adjustPending());
    QVERIFY(!m.adjustSucceeded());
    QVERIFY(!m.adjustFailed());
    QVERIFY(m.adjustStatusText().contains(QStringLiteral("等待 PLC 结果")));

    // M34=1 (调宽执行中): still waiting, never success (spec §10.3 step 6).
    DeviceSnapshotData adjusting = validSnapshotData();
    adjusting.statusWord3 = (quint16(1) << 4); // M34
    model.updateSnapshot(DeviceSnapshot(adjusting));
    QVERIFY(m.adjustPending());
    QVERIFY(!m.adjustSucceeded());
    QVERIFY(m.adjustStatusText().contains(QStringLiteral("等待 PLC 结果")));
}

void RecipeWidthPageTest::successOnlyWhenM44AndD130EqualsAppliedTarget()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    RecipeWidthModel m(model);

    m.beginApply(300);

    // M34=0, M44=1, M45=0, D130 == applied target (300): success
    // (spec §10.3 step 7).
    DeviceSnapshotData ok = validSnapshotData();
    ok.statusWord3 = (quint16(1) << 14); // M44
    ok.currentWidth = 300;               // D130 == applied target
    model.updateSnapshot(DeviceSnapshot(ok));
    QVERIFY(m.adjustSucceeded());
    QVERIFY(!m.adjustFailed());
    QCOMPARE(m.adjustStatusText(), QStringLiteral("调宽成功"));

    // D130 != applied target: NOT success even with M44=1. A fresh apply
    // starts a new wait; the wrong-width snapshot must not derive success.
    m.beginApply(300);
    DeviceSnapshotData wrongWidth = validSnapshotData();
    wrongWidth.statusWord3 = (quint16(1) << 14); // M44
    wrongWidth.currentWidth = 250;               // D130 != 300
    model.updateSnapshot(DeviceSnapshot(wrongWidth));
    QVERIFY(!m.adjustSucceeded());
    QVERIFY(m.adjustPending());

    // M44=1 AND M45=1 (invalid stable state, spec §10.3): never success.
    m.beginApply(300);
    DeviceSnapshotData both = validSnapshotData();
    both.statusWord3 = (quint16(1) << 14) | (quint16(1) << 15); // M44|M45
    both.currentWidth = 300;
    model.updateSnapshot(DeviceSnapshot(both));
    QVERIFY(!m.adjustSucceeded());
    QVERIFY(m.adjustPending());
}

void RecipeWidthPageTest::failureOnM45()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    RecipeWidthModel m(model);

    m.beginApply(300);

    // M34=0, M44=0, M45=1: failure (spec §10.3 step 8).
    DeviceSnapshotData fail = validSnapshotData();
    fail.statusWord3 = (quint16(1) << 15); // M45
    model.updateSnapshot(DeviceSnapshot(fail));
    QVERIFY(m.adjustFailed());
    QVERIFY(!m.adjustSucceeded());
    QCOMPARE(m.adjustStatusText(), QStringLiteral("调宽失败"));
}

void RecipeWidthPageTest::resultDetailFromCoordinatorShown()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    RecipeWidthModel m(model);

    m.beginApply(300);

    // Timeout/failure result from the coordinator is explicit.
    m.setAdjustResult(false, QStringLiteral("调宽等待超时, 请检查设备"));
    QVERIFY(!m.adjustPending());
    QVERIFY(!m.adjustResultOk());
    QVERIFY(m.adjustStatusText().contains(QStringLiteral("调宽等待超时")));

    // Success result from the coordinator (snapshot-confirmed upstream).
    m.setAdjustResult(true, QStringLiteral("调宽完成"));
    QVERIFY(m.adjustResultOk());
    QVERIFY(m.adjustStatusText().contains(QStringLiteral("调宽完成")));
}

// --- page: confirmation and no optimistic success ------------------------------

void RecipeWidthPageTest::applyRequiresSecondConfirmation()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &RecipeWidthPage::applyAdjustRequested);

    // First click arms the confirmation; nothing is sent.
    clickAt(page.applyButton());
    QCOMPARE(spy.count(), 0);
    QCOMPARE(page.applyButton()->text(), QStringLiteral("确认应用?"));

    // Second click confirms and dispatches.
    clickAt(page.applyButton());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toUInt(), quint16(50)); // spin default 50
    QCOMPARE(page.applyButton()->text(), QStringLiteral("应用并调宽"));
}

void RecipeWidthPageTest::applyClickNeverShowsSuccess()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &RecipeWidthPage::applyAdjustRequested);

    clickAt(page.applyButton()); // arm
    clickAt(page.applyButton()); // confirm -> dispatched
    QCOMPARE(spy.count(), 1);

    // No result has been fed: the page must show waiting, never success
    // (spec §11.2: 命令不得乐观更新状态).
    QVERIFY(!page.statusText().contains(QStringLiteral("成功")));
    QVERIFY(page.statusText().contains(QStringLiteral("等待 PLC 结果")));
}

void RecipeWidthPageTest::pageTargetEqualsCurrentDoesNotEmitApply()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    DeviceSnapshotData d = validSnapshotData();
    d.currentWidth = 150; // D130
    model.updateSnapshot(DeviceSnapshot(d));
    QSignalSpy spy(&page, &RecipeWidthPage::applyAdjustRequested);

    // Edited width equals the current width: no command is sent even after
    // two clicks (spec §10.3 step 3: 显示"当前已是目标宽度"并结束).
    page.widthSpin()->setValue(150);
    clickAt(page.applyButton());
    clickAt(page.applyButton());
    QCOMPARE(spy.count(), 0);
    QVERIFY(page.statusText().contains(QStringLiteral("当前已是目标宽度")));
}

void RecipeWidthPageTest::synchronousResultDuringApplyNotSwallowed()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // Task 20 wires the coordinator with a DIRECT connection: a synchronous
    // rejection (e.g. interlock changed at click time) calls setAdjustResult
    // inside the emit. The result must survive beginApply (regression: emit
    // before beginApply swallowed it and left the UI stuck pending).
    QObject::connect(&page, &RecipeWidthPage::applyAdjustRequested, &page,
                     [&page](quint16) {
                         page.setAdjustResult(false,
                                              QStringLiteral("互锁已变化, 拒绝"));
                     });

    clickAt(page.applyButton()); // arm
    clickAt(page.applyButton()); // confirm -> emit -> synchronous reject

    QVERIFY(!page.statusText().contains(QStringLiteral("等待 PLC 结果")));
    QVERIFY(page.statusText().contains(QStringLiteral("互锁已变化, 拒绝")));
}

void RecipeWidthPageTest::armedStateResetOnPageSwitch()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    RecipeWidthPage *page = w.findChild<RecipeWidthPage *>();
    QVERIFY(page != nullptr);
    w.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    w.setCurrentPage(1);
    QVERIFY(page->isVisible());
    clickAt(page->applyButton()); // arm
    QCOMPARE(page->applyButton()->text(), QStringLiteral("确认应用?"));

    // Switching away and back clears the armed confirmation (spec §11.1-§11.2
    // 页面切换清零意图): the next click re-arms instead of dispatching.
    w.setCurrentPage(0);
    QVERIFY(!page->isVisible());
    w.setCurrentPage(1);
    QVERIFY(page->isVisible());
    QCOMPARE(page->applyButton()->text(), QStringLiteral("应用并调宽"));

    QSignalSpy spy(page, &RecipeWidthPage::applyAdjustRequested);
    clickAt(page->applyButton());
    QCOMPARE(spy.count(), 0); // re-armed, nothing dispatched
    QCOMPARE(page->applyButton()->text(), QStringLiteral("确认应用?"));
}

void RecipeWidthPageTest::armedStateResetOnGateChange()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    clickAt(page.applyButton()); // arm
    QCOMPARE(page.applyButton()->text(), QStringLiteral("确认应用?"));

    // A gate change that disables apply (M34=1 调宽中) resets the armed
    // confirmation; re-enabling later requires a fresh confirm.
    DeviceSnapshotData adjusting = validSnapshotData();
    adjusting.statusWord3 = (quint16(1) << 4); // M34
    model.updateSnapshot(DeviceSnapshot(adjusting));
    QVERIFY(!page.applyButton()->isEnabled());
    QCOMPARE(page.applyButton()->text(), QStringLiteral("应用并调宽"));

    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QVERIFY(page.applyButton()->isEnabled());
    QSignalSpy spy(&page, &RecipeWidthPage::applyAdjustRequested);
    clickAt(page.applyButton());
    QCOMPARE(spy.count(), 0); // re-armed, nothing dispatched
    QCOMPARE(page.applyButton()->text(), QStringLiteral("确认应用?"));
}

void RecipeWidthPageTest::setRecipesClearsStaleEditorState()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    page.setRecipes({recipe(1, QStringLiteral("窄幅"), 180)});
    page.recipeList()->setCurrentRow(0); // loads 窄幅/180 into the editor
    QCOMPARE(page.nameEdit()->text(), QStringLiteral("窄幅"));
    QCOMPARE(page.widthSpin()->value(), 180);

    // Reloading the list clears the stale editor/selection state.
    page.setRecipes({recipe(2, QStringLiteral("宽幅"), 350)});
    QCOMPARE(page.recipeList()->currentRow(), -1);
    QVERIFY(page.nameEdit()->text().isEmpty());
    QCOMPARE(page.widthSpin()->value(), 50);
}

void RecipeWidthPageTest::saveRejectsEmptyName()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QSignalSpy spy(&page, &RecipeWidthPage::saveRecipeRequested);

    // Empty name: no save request is emitted.
    page.nameEdit()->clear();
    clickAt(page.saveButton());
    QCOMPARE(spy.count(), 0);

    // Whitespace-only name: still rejected.
    page.nameEdit()->setText(QStringLiteral("   "));
    clickAt(page.saveButton());
    QCOMPARE(spy.count(), 0);

    // Valid name: request emitted with the trimmed name.
    page.nameEdit()->setText(QStringLiteral("  窄幅  "));
    clickAt(page.saveButton());
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("窄幅"));
}

// --- page: rendering and permission gating -------------------------------------

void RecipeWidthPageTest::pageShowsStatusAndDisplays()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    QCOMPARE(page.fieldDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("200 mm"));   // D128
    QCOMPARE(page.fieldDisplay(QStringLiteral("currentWidth"))->text(),
             QStringLiteral("150 mm"));   // D130
    QCOMPARE(page.fieldDisplay(QStringLiteral("widthDelta"))->text(),
             QStringLiteral("50 mm"));    // D210
    QCOMPARE(page.fieldDisplay(QStringLiteral("pulsePerMm"))->text(),
             QStringLiteral("1280 脉冲/mm")); // D204
    QCOMPARE(page.fieldDisplay(QStringLiteral("widthSpeed"))->text(),
             QStringLiteral("15 mm/s"));  // D220
    QCOMPARE(page.statusText(), QStringLiteral("空闲"));
}

void RecipeWidthPageTest::operatorSeesDisabledApplyWithReason()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("operator"), Role::Operator);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // 操作员只读: apply + recipe CRUD disabled with the permission reason
    // (spec §11.4: 无权限操作应保持可发现但禁用，并说明所需权限).
    QVERIFY(!page.applyButton()->isEnabled());
    QVERIFY(page.applyButton()->toolTip().contains(
        QStringLiteral("需要管理员权限")));
    QVERIFY(!page.saveButton()->isEnabled());
    QVERIFY(!page.deleteButton()->isEnabled());
    QVERIFY(!page.nameEdit()->isEnabled());
    QVERIFY(!page.widthSpin()->isEnabled());
}

void RecipeWidthPageTest::editorControlsMeetTouchTargetSize()
{
    // 触摸目标 >= 48 px (spec §11.1): the interactive editor controls must be
    // at least as tall as every other control on the page (buttons 48, apply
    // 64, displays 48, status 48).
    ShellModel model;
    RecipeWidthPage page(model);

    QVERIFY(page.nameEdit()->minimumHeight() >= 48);
    QVERIFY(page.widthSpin()->minimumHeight() >= 48);
}

// --- MainWindow integration ------------------------------------------------------

void RecipeWidthPageTest::mainWindowUsesRecipeWidthPage()
{
    // The 配方与调宽 stub is replaced by the real RecipeWidthPage at index 1.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    RecipeWidthPage *page = w.findChild<RecipeWidthPage *>();
    QVERIFY(page != nullptr);
    w.setCurrentPage(1);
    QCOMPARE(w.currentPageIndex(), 1);

    w.shellModel()->setUser(QStringLiteral("admin"), Role::Admin);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();
    QVERIFY(page->isVisible());
    QCOMPARE(page->fieldDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("200 mm"));
}

QTEST_MAIN(RecipeWidthPageTest)
#include "test_recipe_width_page.moc"
