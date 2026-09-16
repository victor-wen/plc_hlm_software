// PLC-HMI-002 developer-owned unit/regression tests (D8) for the recipe
// editor-preservation and page-local database feedback implementation.
//
// These tests complement the independent black-box targets
// (recipe_editor_state_test.cpp, recipe_result_feedback_test.cpp) and focus on
// the edges the contract calls out explicitly:
//   D1 selection matched by record id, not row; editors preserved on reload.
//   D2/D3 pending/result text, control gating and survival across reloads.
//   D4 forced (handler-level) rejections are visible, never silent.
//   D5 inline editor reason follows the role and is never hidden.
//   D6 administrator save/delete work without any PLC snapshot.
//   D7 no duplicate request while pending; latest page-local event wins.
//   Page-local results are not projected into the machine command status.

#include <QtTest>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSpinBox>

#include "domain/operator_command_status.h"
#include "ports/repositories.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

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

class RecipeEditorDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    // --- D1: preservation is keyed on the selected record id ------------------
    void reloadPreservesSelectionByIdWhenRowsReorder();
    void reloadAfterConfirmedDeletionClearsSelectionAndEditors();

    // --- D2/D3: pending, result and reload survival ----------------------------
    void savePendingDisablesControlAndBlocksDuplicateRequest();
    void deletePendingDisablesControlAndBlocksDuplicateRequest();
    void saveFailureTextSurvivesRepeatedReloadsAndEditorsStayUsable();
    void deleteSuccessShowsDeletedAndReloadClearsSelectionOnly();
    void deleteFailureKeepsSelectionAndUnsavedDraftAcrossReload();

    // --- D4: handler-level rejection is visible --------------------------------
    void forcedNonAdminSaveAndDeleteAreRejectedVisiblyWithoutRequest();

    // --- D5: inline editor reason ----------------------------------------------
    void editorReasonLabelFollowsRoleAndIsNeverHidden();

    // --- D6: administrator editing is database-only ----------------------------
    void adminCanDispatchSaveAndDeleteWithoutAnySnapshot();

    // --- D7 + page-local-only results ------------------------------------------
    void latestPageLocalEventWinsInStatusText();
    void recipeResultsDoNotEnterMachineCommandStatusProjection();
};

// --- D1 ------------------------------------------------------------------------

void RecipeEditorDeveloperTest::reloadPreservesSelectionByIdWhenRowsReorder()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    const RecipeRecord narrow = recipe(1, QStringLiteral("窄幅"), 180);
    const RecipeRecord wide = recipe(2, QStringLiteral("宽幅"), 350);
    page.setRecipes({narrow, wide});
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    QCOMPARE(page.recipeList()->currentRow(), 0);

    page.nameEdit()->setText(QStringLiteral("未保存草稿"));
    page.widthSpin()->setValue(222);

    // Same records, different row order: the selection must follow the record
    // id, not the row index, and the draft must survive.
    page.setRecipes({wide, narrow});
    QApplication::processEvents();

    QCOMPARE(page.recipeList()->count(), 2);
    QCOMPARE(page.recipeList()->currentRow(), 1);
    QCOMPARE(page.nameEdit()->text(), QStringLiteral("未保存草稿"));
    QCOMPARE(page.widthSpin()->value(), 222);
}

void RecipeEditorDeveloperTest::reloadAfterConfirmedDeletionClearsSelectionAndEditors()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    page.setRecipes({recipe(1, QStringLiteral("窄幅"), 180)});
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    page.nameEdit()->setText(QStringLiteral("待删除草稿"));
    page.widthSpin()->setValue(321);

    // The selected record id is gone: selection and editors reset.
    page.setRecipes({recipe(2, QStringLiteral("宽幅"), 350)});
    QApplication::processEvents();

    QCOMPARE(page.recipeList()->currentRow(), -1);
    QVERIFY(page.nameEdit()->text().isEmpty());
    QCOMPARE(page.widthSpin()->value(), 50);
}

// --- D2/D3 ---------------------------------------------------------------------

void RecipeEditorDeveloperTest::savePendingDisablesControlAndBlocksDuplicateRequest()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);
    page.nameEdit()->setText(QStringLiteral("配方A"));

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);

    page.setRecipeSavePending();
    QVERIFY2(!page.statusText().trimmed().isEmpty(),
             "a pending save must be visible");
    QVERIFY2(!page.statusText().contains(QStringLiteral("已保存")),
             "a pending save must not claim success");
    QVERIFY2(!page.saveButton()->isEnabled(),
             "the save control must be disabled while a result is pending");

    clickAt(page.saveButton());
    QCOMPARE(saveSpy.count(), 0);

    // The terminal result re-enables the control and the next attempt emits
    // exactly one request carrying the trimmed name.
    page.setRecipeSaveResult(true, QString());
    QVERIFY(page.saveButton()->isEnabled());
    clickAt(page.saveButton());
    QCOMPARE(saveSpy.count(), 1);
    QCOMPARE(saveSpy.first().at(0).toString(), QStringLiteral("配方A"));
}

void RecipeEditorDeveloperTest::deletePendingDisablesControlAndBlocksDuplicateRequest()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);
    page.setRecipes({recipe(7, QStringLiteral("配方"), 200)});
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();

    QSignalSpy deleteSpy(&page, &RecipeWidthPage::deleteRecipeRequested);

    page.setRecipeDeletePending();
    QVERIFY2(!page.statusText().trimmed().isEmpty(),
             "a pending delete must be visible");
    QVERIFY2(!page.deleteButton()->isEnabled(),
             "the delete control must be disabled while a result is pending");

    clickAt(page.deleteButton());
    QCOMPARE(deleteSpy.count(), 0);

    page.setRecipeDeleteResult(true, QString());
    QVERIFY(page.deleteButton()->isEnabled());
    clickAt(page.deleteButton());
    QCOMPARE(deleteSpy.count(), 1);
    QCOMPARE(deleteSpy.first().at(0).toLongLong(), qint64(7));
}

void RecipeEditorDeveloperTest::saveFailureTextSurvivesRepeatedReloadsAndEditorsStayUsable()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    const QString detail = QStringLiteral("数据库写入失败: disk I/O error");
    page.setRecipeSaveResult(false, detail);

    for (int i = 0; i < 3; ++i) {
        page.setRecipes({recipe(1, QStringLiteral("窄幅"), 180)});
        QApplication::processEvents();
    }

    QVERIFY2(page.statusText().contains(detail),
             qPrintable(QStringLiteral("status was '%1'").arg(page.statusText())));
    QVERIFY(!page.statusText().contains(QStringLiteral("已保存")));
    // A failed database operation keeps the editors usable (D6-style
    // database-only operation, no PLC freshness requirement).
    QVERIFY(page.nameEdit()->isEnabled());
    QVERIFY(page.widthSpin()->isEnabled());
}

void RecipeEditorDeveloperTest::deleteSuccessShowsDeletedAndReloadClearsSelectionOnly()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    page.setRecipes({recipe(1, QStringLiteral("待删除"), 180)});
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    page.nameEdit()->setText(QStringLiteral("未保存草稿"));
    page.widthSpin()->setValue(300);

    page.setRecipeDeletePending();
    page.setRecipeDeleteResult(true, QString());
    QVERIFY(page.statusText().contains(QStringLiteral("已删除")));

    // Confirmed deletion reload: the selected record is gone, so selection and
    // editors reset, while the delete result stays visible (D1 + D3).
    page.setRecipes({});
    QApplication::processEvents();

    QCOMPARE(page.recipeList()->currentRow(), -1);
    QVERIFY(page.nameEdit()->text().isEmpty());
    QCOMPARE(page.widthSpin()->value(), 50);
    QVERIFY2(page.statusText().contains(QStringLiteral("已删除")),
             "the reload discarded the delete result text");
}

void RecipeEditorDeveloperTest::deleteFailureKeepsSelectionAndUnsavedDraftAcrossReload()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    const RecipeRecord record = recipe(1, QStringLiteral("窄幅"), 180);
    page.setRecipes({record});
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    page.nameEdit()->setText(QStringLiteral("修改中"));
    page.widthSpin()->setValue(260);

    page.setRecipeDeleteResult(false, QStringLiteral("recipe not found"));

    // The record still exists, so the reload must not drop the selection or
    // the user's draft; the failure detail stays visible.
    page.setRecipes({record});
    QApplication::processEvents();

    QCOMPARE(page.recipeList()->currentRow(), 0);
    QCOMPARE(page.nameEdit()->text(), QStringLiteral("修改中"));
    QCOMPARE(page.widthSpin()->value(), 260);
    QVERIFY(page.statusText().contains(QStringLiteral("recipe not found")));
    QVERIFY(!page.statusText().contains(QStringLiteral("已删除")));
}

// --- D4 ------------------------------------------------------------------------

void RecipeEditorDeveloperTest::forcedNonAdminSaveAndDeleteAreRejectedVisiblyWithoutRequest()
{
    ShellModel model;
    model.setUser(QStringLiteral("operator1"), Role::Operator);
    RecipeWidthPage page(model);
    page.setRecipes({recipe(1, QStringLiteral("配方"), 180)});
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    QSignalSpy deleteSpy(&page, &RecipeWidthPage::deleteRecipeRequested);

    // The real controls are disabled for a non-admin; force the click through
    // to prove the handler itself is not a silent no-op (D4).
    page.saveButton()->setEnabled(true);
    page.deleteButton()->setEnabled(true);
    page.nameEdit()->setText(QStringLiteral("越权草稿"));
    const QString before = page.statusText();

    clickAt(page.saveButton());
    QCOMPARE(saveSpy.count(), 0);
    QVERIFY2(!page.statusText().trimmed().isEmpty()
                 && page.statusText() != before,
             qPrintable(QStringLiteral("status stayed '%1'").arg(before)));
    QVERIFY(page.statusText().contains(QStringLiteral("管理员")));

    clickAt(page.deleteButton());
    QCOMPARE(deleteSpy.count(), 0);
    QVERIFY(page.statusText().contains(QStringLiteral("管理员")));
}

// --- D5 ------------------------------------------------------------------------

void RecipeEditorDeveloperTest::editorReasonLabelFollowsRoleAndIsNeverHidden()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    QLabel *reason = page.editorReasonLabel();
    QVERIFY(reason != nullptr);
    QVERIFY2(!reason->isHidden(),
             "the inline editor reason must never be hidden");
    QVERIFY(reason->text().trimmed().isEmpty());
    QVERIFY(page.nameEdit()->isEnabled());
    QVERIFY(page.widthSpin()->isEnabled());

    model.setUser(QStringLiteral("operator1"), Role::Operator);
    QApplication::processEvents();

    QVERIFY(!page.nameEdit()->isEnabled());
    QVERIFY(!page.widthSpin()->isEnabled());
    QVERIFY(!reason->isHidden());
    QVERIFY2(reason->text().contains(QStringLiteral("管理员")),
             qPrintable(QStringLiteral("reason text was '%1'")
                            .arg(reason->text())));
}

// --- D6 ------------------------------------------------------------------------

void RecipeEditorDeveloperTest::adminCanDispatchSaveAndDeleteWithoutAnySnapshot()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QVERIFY2(!model.snapshotFresh(),
             "test precondition: no snapshot was published");
    RecipeWidthPage page(model);

    page.setRecipes({recipe(3, QStringLiteral("离线配方"), 250)});
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    QVERIFY(page.nameEdit()->text() == QStringLiteral("离线配方"));

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    QSignalSpy deleteSpy(&page, &RecipeWidthPage::deleteRecipeRequested);

    page.nameEdit()->setText(QStringLiteral("离线配方"));
    clickAt(page.saveButton());
    QCOMPARE(saveSpy.count(), 1);
    page.setRecipeSaveResult(true, QString());

    clickAt(page.deleteButton());
    QCOMPARE(deleteSpy.count(), 1);
    QCOMPARE(deleteSpy.first().at(0).toLongLong(), qint64(3));
}

// --- D7 / page-local-only ------------------------------------------------------

void RecipeEditorDeveloperTest::latestPageLocalEventWinsInStatusText()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    page.setRecipeSaveResult(true, QString());
    QVERIFY(page.statusText().contains(QStringLiteral("已保存")));

    // A later adjust verdict takes over the shared page status line.
    page.setAdjustResult(false, QStringLiteral("调宽等待超时"));
    QVERIFY(page.statusText().contains(QStringLiteral("超时")));

    // A later recipe result takes it back.
    page.setRecipeSaveResult(false, QStringLiteral("唯一名称冲突"));
    QVERIFY(page.statusText().contains(QStringLiteral("唯一名称冲突")));
    QVERIFY(!page.statusText().contains(QStringLiteral("已保存")));
}

void RecipeEditorDeveloperTest::recipeResultsDoNotEnterMachineCommandStatusProjection()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    page.setRecipeSavePending();
    page.setRecipeSaveResult(true, QString());
    page.setRecipeDeletePending();
    page.setRecipeDeleteResult(false, QStringLiteral("删除失败"));

    const OperatorCommandStatus status = model.operatorCommandStatus();
    QCOMPARE(status.command, Command::Count);
    QCOMPARE(status.lifecycle_state, OperatorCommandState::Idle);
}

QTEST_MAIN(RecipeEditorDeveloperTest)
#include "recipe_editor_developer_test.moc"
