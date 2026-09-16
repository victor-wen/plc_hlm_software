// PLC-HMI-002 black-box tests: recipe editor state preservation across
// asynchronous list reloads, visible page-local rejection for silent no-op
// attempts, administrator editability while the PLC is offline, adjust-verdict
// preservation, and no focus/content theft during reloads
// (brief OB-1, OB-2, OB-5, OB-7, OB-8, OB-9).
//
// This file deliberately uses only APIs that already exist on the tree, so the
// target compiles and the expected RED is an observable runtime assertion
// failure. The new pending/result API introduced by the change is covered
// separately in recipe_result_feedback_test.cpp, whose compile failure is the
// expected RED for that target.

#include <QtTest>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSpinBox>
#include <QVector>

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

// One value-initialized recipe record. The assertions below are about the
// observable preservation/removal/editability behavior and intentionally do not
// depend on any record field value, so no field is named or guessed.
QVector<RecipeRecord> oneRecipe()
{
    QVector<RecipeRecord> recipes;
    recipes.append(RecipeRecord{});
    return recipes;
}

} // namespace

class RecipeEditorStateTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1: reload containing the selected recipe preserves unsaved input --
    void reloadWithSameSelectionPreservesUnsavedEditorContent();
    void repeatedReloadsKeepSelectionAndUnsavedEditsStable();

    // --- OB-2: confirmed deletion clears selection and editors ----------------
    void reloadWithoutSelectedRecordClearsSelectionAndEditors();

    // --- OB-5: no silent no-op remains ----------------------------------------
    void saveWithEmptyNameIsRejectedVisiblyWithoutRequest();
    void saveWithWhitespaceOnlyNameIsRejectedVisiblyWithoutRequest();
    void saveWithTabAndSpaceOnlyNameIsRejectedVisiblyWithoutRequest();
    void rejectedSaveThenValidSaveEmitsRequest();
    void deleteWithoutSelectionIsRejectedVisiblyWithoutRequest();

    // --- OB-7: administrator editors stay usable without PLC freshness --------
    void adminEditorsStayUsableOfflineWithoutFreshSnapshot();

    // --- OB-8: reloads do not alter a delivered adjust verdict ----------------
    void adjustVerdictSurvivesReloadWithSameSelection();
    void adjustVerdictSurvivesReloadThatRemovesSelection();

    // --- OB-9: reload between keystrokes keeps content and focus --------------
    void reloadBetweenKeystrokesPreservesTypedTextAndFocus();
    void reloadWithNonEmptyListWhileTypingWithoutSelectionKeepsTextAndFocus();
};

// --- OB-1 ---------------------------------------------------------------------

void RecipeEditorStateTest::reloadWithSameSelectionPreservesUnsavedEditorContent()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QVector<RecipeRecord> recipes = oneRecipe();
    page.setRecipes(recipes);
    QCOMPARE(page.recipeList()->count(), 1);
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    QCOMPARE(page.recipeList()->currentRow(), 0);

    // Unsaved edits typed by the user after selecting the recipe.
    page.nameEdit()->setText(QStringLiteral("草稿名称"));
    page.widthSpin()->setValue(321);

    // Asynchronous reload whose list still contains the selected recipe.
    page.setRecipes(recipes);
    QApplication::processEvents();

    QCOMPARE(page.recipeList()->currentRow(), 0);
    QVERIFY(page.recipeList()->currentItem() != nullptr);
    QCOMPARE(page.nameEdit()->text(), QStringLiteral("草稿名称"));
    QCOMPARE(page.widthSpin()->value(), 321);
}

void RecipeEditorStateTest::repeatedReloadsKeepSelectionAndUnsavedEditsStable()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QVector<RecipeRecord> recipes = oneRecipe();
    page.setRecipes(recipes);
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();

    page.nameEdit()->setText(QStringLiteral("连续刷新草稿"));
    page.widthSpin()->setValue(288);

    for (int i = 0; i < 3; ++i) {
        page.setRecipes(recipes);
        QApplication::processEvents();
    }

    QCOMPARE(page.recipeList()->currentRow(), 0);
    QCOMPARE(page.nameEdit()->text(), QStringLiteral("连续刷新草稿"));
    QCOMPARE(page.widthSpin()->value(), 288);
}

// --- OB-2 ---------------------------------------------------------------------

void RecipeEditorStateTest::reloadWithoutSelectedRecordClearsSelectionAndEditors()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QVector<RecipeRecord> recipes = oneRecipe();
    page.setRecipes(recipes);
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    QCOMPARE(page.recipeList()->currentRow(), 0);

    page.nameEdit()->setText(QStringLiteral("将被确认删除"));
    page.widthSpin()->setValue(321);

    // Confirmed deletion: the reloaded list no longer contains the selection.
    page.setRecipes(QVector<RecipeRecord>{});
    QApplication::processEvents();

    QCOMPARE(page.recipeList()->count(), 0);
    QCOMPARE(page.recipeList()->currentRow(), -1);
    QVERIFY(page.nameEdit()->text().isEmpty());
    QVERIFY2(page.widthSpin()->value() != 321,
             "a confirmed deletion must reset the width editor to a neutral value");
}

// --- OB-5 ---------------------------------------------------------------------

void RecipeEditorStateTest::saveWithEmptyNameIsRejectedVisiblyWithoutRequest()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    page.nameEdit()->clear();
    const QString before = page.statusText();
    clickAt(page.saveButton());

    QCOMPARE(saveSpy.count(), 0);
    const QString after = page.statusText();
    QVERIFY2(!after.trimmed().isEmpty() && after != before,
             qPrintable(QStringLiteral("saving with an empty name must produce "
                                       "a visible page-local reason (status "
                                       "stayed '%1')")
                            .arg(before)));
}

void RecipeEditorStateTest::saveWithWhitespaceOnlyNameIsRejectedVisiblyWithoutRequest()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    page.nameEdit()->setText(QStringLiteral("   "));
    const QString before = page.statusText();
    clickAt(page.saveButton());

    QCOMPARE(saveSpy.count(), 0);
    const QString after = page.statusText();
    QVERIFY2(!after.trimmed().isEmpty() && after != before,
             qPrintable(QStringLiteral("saving with a whitespace-only name must "
                                       "produce a visible page-local reason "
                                       "(status stayed '%1')")
                            .arg(before)));
}

void RecipeEditorStateTest::saveWithTabAndSpaceOnlyNameIsRejectedVisiblyWithoutRequest()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    // Mixed whitespace (spaces plus a tab) is still whitespace-only.
    page.nameEdit()->setText(QStringLiteral(" \t "));
    const QString before = page.statusText();
    clickAt(page.saveButton());

    QCOMPARE(saveSpy.count(), 0);
    const QString after = page.statusText();
    QVERIFY2(!after.trimmed().isEmpty() && after != before,
             qPrintable(QStringLiteral("saving with a tab/space-only name must "
                                       "produce a visible page-local reason "
                                       "(status stayed '%1')")
                            .arg(before)));
}

void RecipeEditorStateTest::rejectedSaveThenValidSaveEmitsRequest()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);

    // A rejected attempt must not latch: after the visible rejection, a valid
    // name submits normally.
    page.nameEdit()->clear();
    clickAt(page.saveButton());
    QCOMPARE(saveSpy.count(), 0);

    page.nameEdit()->setText(QStringLiteral("恢复后保存"));
    clickAt(page.saveButton());
    QCOMPARE(saveSpy.count(), 1);
}

void RecipeEditorStateTest::deleteWithoutSelectionIsRejectedVisiblyWithoutRequest()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    QSignalSpy deleteSpy(&page, &RecipeWidthPage::deleteRecipeRequested);
    QCOMPARE(page.recipeList()->count(), 0);
    const QString before = page.statusText();
    clickAt(page.deleteButton());

    QCOMPARE(deleteSpy.count(), 0);
    const QString after = page.statusText();
    QVERIFY2(!after.trimmed().isEmpty() && after != before,
             qPrintable(QStringLiteral("deleting without a selection must "
                                       "produce a visible page-local reason "
                                       "(status stayed '%1')")
                            .arg(before)));
}

// --- OB-7 ---------------------------------------------------------------------

void RecipeEditorStateTest::adminEditorsStayUsableOfflineWithoutFreshSnapshot()
{
    ShellModel model;
    model.setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage page(model);

    // No snapshot was ever published: the model is offline and has no fresh
    // data, but recipe editing is a database operation and must stay usable.
    QVERIFY(page.nameEdit()->isEnabled());
    QVERIFY(page.widthSpin()->isEnabled());

    page.nameEdit()->setText(QStringLiteral("离线配方"));
    page.widthSpin()->setValue(250);

    QCOMPARE(page.nameEdit()->text(), QStringLiteral("离线配方"));
    QCOMPARE(page.widthSpin()->value(), 250);
}

// --- OB-8 ---------------------------------------------------------------------

void RecipeEditorStateTest::adjustVerdictSurvivesReloadWithSameSelection()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QVector<RecipeRecord> recipes = oneRecipe();
    page.setRecipes(recipes);
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();

    page.setAdjustResult(false, QStringLiteral("调宽等待超时, 请检查设备"));
    QVERIFY(page.statusText().contains(QStringLiteral("超时")));

    page.setRecipes(recipes);
    QApplication::processEvents();

    QVERIFY2(page.statusText().contains(QStringLiteral("超时")),
             "a recipe list reload discarded the adjust-width verdict");
    QVERIFY(!page.statusText().contains(QStringLiteral("调宽成功")));
}

void RecipeEditorStateTest::adjustVerdictSurvivesReloadThatRemovesSelection()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QVector<RecipeRecord> recipes = oneRecipe();
    page.setRecipes(recipes);
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();

    page.setAdjustResult(true, QStringLiteral("调宽完成"));
    QVERIFY(page.statusText().contains(QStringLiteral("调宽完成")));

    // The selected record is removed by the reload; the editors reset, but the
    // already-delivered verdict must remain visible.
    page.setRecipes(QVector<RecipeRecord>{});
    QApplication::processEvents();

    QVERIFY2(page.statusText().contains(QStringLiteral("调宽完成")),
             "a selection-clearing reload discarded the adjust-width verdict");
}

// --- OB-9 ---------------------------------------------------------------------

void RecipeEditorStateTest::reloadBetweenKeystrokesPreservesTypedTextAndFocus()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.show();
    QApplication::processEvents();
    page.nameEdit()->setFocus(Qt::MouseFocusReason);
    QApplication::processEvents();
    QVERIFY2(page.nameEdit()->hasFocus(),
             "test precondition: the name edit could not take focus");

    page.nameEdit()->setText(QStringLiteral("正在输入"));

    // A reload arrives between keystrokes.
    page.setRecipes(QVector<RecipeRecord>{});
    QApplication::processEvents();

    QCOMPARE(page.nameEdit()->text(), QStringLiteral("正在输入"));
    QVERIFY2(page.nameEdit()->hasFocus(),
             "a recipe list reload stole focus from the name edit");
}

void RecipeEditorStateTest::reloadWithNonEmptyListWhileTypingWithoutSelectionKeepsTextAndFocus()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.show();
    QApplication::processEvents();
    page.nameEdit()->setFocus(Qt::MouseFocusReason);
    QApplication::processEvents();
    QVERIFY2(page.nameEdit()->hasFocus(),
             "test precondition: the name edit could not take focus");

    // The user is typing with no row selected.
    page.nameEdit()->setText(QStringLiteral("无选择草稿"));
    QCOMPARE(page.recipeList()->currentRow(), -1);

    // A reload arrives between keystrokes and now contains a record; it must
    // not steal focus or overwrite the in-progress editor content.
    page.setRecipes(oneRecipe());
    QApplication::processEvents();

    QCOMPARE(page.nameEdit()->text(), QStringLiteral("无选择草稿"));
    QVERIFY2(page.nameEdit()->hasFocus(),
             "a recipe list reload stole focus while the user was typing "
             "without a selection");
}

QTEST_MAIN(RecipeEditorStateTest)
#include "recipe_editor_state_test.moc"
