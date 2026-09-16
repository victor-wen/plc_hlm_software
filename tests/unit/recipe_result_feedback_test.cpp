// PLC-HMI-002 black-box tests: page-local save/delete pending, success and
// failure feedback, duplicate-request prevention while pending, and the inline
// disabled-editor reason (brief OB-3, OB-4, OB-5, OB-6, OB-7).
//
// This file depends on the contract API introduced by the change
// (setRecipeSavePending/setRecipeSaveResult, setRecipeDeletePending/
// setRecipeDeleteResult, editorReasonLabel), so a compile failure of this target
// before implementation is the expected RED and is isolated here.

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
// observable feedback behavior and intentionally do not depend on any record
// field value, so no field is named or guessed.
QVector<RecipeRecord> oneRecipe()
{
    QVector<RecipeRecord> recipes;
    recipes.append(RecipeRecord{});
    return recipes;
}

} // namespace

class RecipeResultFeedbackTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-3: save pending / success / failure feedback ----------------------
    void savePendingStateIsVisibleBeforeResult();
    void saveSuccessShowsSavedAndSurvivesReload();
    void saveSuccessSurvivesRepeatedReloads();
    void saveFailureShowsErrorDetailAndSurvivesReload();
    void saveFailureDetailSurvivesRepeatedReloads();
    void saveSuccessThenReloadThenFailureShowsFailure();
    void saveFailureThenSuccessfulSaveShowsSaved();

    // --- OB-4: delete pending / success / failure feedback --------------------
    void deletePendingStateIsVisibleBeforeResult();
    void deleteSuccessShowsDeleted();
    void deleteFailureShowsErrorDetail();
    void deleteFailureDetailSurvivesRepeatedReloads();
    void deleteSuccessThenFailureShowsErrorDetail();

    // --- OB-6: pending blocks duplicates and the attempt is never silent ------
    void savePendingBlocksDuplicateRequestAndStaysVisible();
    void deletePendingBlocksDuplicateRequestAndStaysVisible();
    void saveControlIsUsableAgainAfterResult();

    // --- OB-5 / OB-7: non-admin roles get disabled editors and a visible
    //     inline reason (never tooltip-only), and no request is emitted --------
    void operatorRoleDisablesEditorsWithInlineAdminReason();
    void anonymousRoleDisablesEditorsWithInlineAdminReason();
};

// --- OB-3 ---------------------------------------------------------------------

void RecipeResultFeedbackTest::savePendingStateIsVisibleBeforeResult()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipeSavePending();

    const QString pending = page.statusText();
    QVERIFY2(!pending.trimmed().isEmpty(),
             "a pending save produced no visible page-local status");
    QVERIFY2(!pending.contains(QStringLiteral("已保存")),
             "a pending save must not claim success");
}

void RecipeResultFeedbackTest::saveSuccessShowsSavedAndSurvivesReload()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipeSaveResult(true, QString());

    QVERIFY2(page.statusText().contains(QStringLiteral("已保存")),
             qPrintable(QStringLiteral("save success status was '%1'")
                            .arg(page.statusText())));

    page.setRecipes(oneRecipe());
    QApplication::processEvents();

    QVERIFY2(page.statusText().contains(QStringLiteral("已保存")),
             "a later recipe list reload discarded the save result text");
}

void RecipeResultFeedbackTest::saveSuccessSurvivesRepeatedReloads()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipeSaveResult(true, QString());

    for (int i = 0; i < 3; ++i) {
        page.setRecipes(oneRecipe());
        QApplication::processEvents();
    }

    QVERIFY2(page.statusText().contains(QStringLiteral("已保存")),
             "repeated recipe list reloads discarded the save success text");
}

void RecipeResultFeedbackTest::saveFailureShowsErrorDetailAndSurvivesReload()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QString detail = QStringLiteral("数据库写入失败: disk I/O error");
    page.setRecipeSaveResult(false, detail);

    QVERIFY2(page.statusText().contains(detail),
             qPrintable(QStringLiteral("save failure status was '%1'")
                            .arg(page.statusText())));
    QVERIFY(!page.statusText().contains(QStringLiteral("已保存")));

    page.setRecipes(oneRecipe());
    QApplication::processEvents();

    QVERIFY2(page.statusText().contains(detail),
             "a later recipe list reload discarded the save failure detail");
}

void RecipeResultFeedbackTest::saveFailureDetailSurvivesRepeatedReloads()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QString detail = QStringLiteral("数据库写入失败: 重试仍失败");
    page.setRecipeSaveResult(false, detail);

    for (int i = 0; i < 3; ++i) {
        page.setRecipes(oneRecipe());
        QApplication::processEvents();
    }

    QVERIFY2(page.statusText().contains(detail),
             "repeated recipe list reloads discarded the save failure detail");
}

void RecipeResultFeedbackTest::saveSuccessThenReloadThenFailureShowsFailure()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipeSaveResult(true, QString());
    QVERIFY(page.statusText().contains(QStringLiteral("已保存")));

    page.setRecipes(oneRecipe());
    QApplication::processEvents();

    const QString detail = QStringLiteral("第二次保存失败: 唯一名称冲突");
    page.setRecipeSaveResult(false, detail);

    QVERIFY2(page.statusText().contains(detail),
             qPrintable(QStringLiteral("later save failure status was '%1'")
                            .arg(page.statusText())));
}

void RecipeResultFeedbackTest::saveFailureThenSuccessfulSaveShowsSaved()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QString detail = QStringLiteral("第一次保存失败: 磁盘只读");
    page.setRecipeSaveResult(false, detail);
    QVERIFY(page.statusText().contains(detail));

    page.setRecipeSaveResult(true, QString());

    QVERIFY2(page.statusText().contains(QStringLiteral("已保存")),
             qPrintable(QStringLiteral("a save success after a failure did not "
                                       "show 已保存 (status was '%1')")
                            .arg(page.statusText())));
    QVERIFY(!page.statusText().contains(QStringLiteral("已删除")));
}

// --- OB-4 ---------------------------------------------------------------------

void RecipeResultFeedbackTest::deletePendingStateIsVisibleBeforeResult()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipeDeletePending();

    const QString pending = page.statusText();
    QVERIFY2(!pending.trimmed().isEmpty(),
             "a pending delete produced no visible page-local status");
    QVERIFY2(!pending.contains(QStringLiteral("已删除")),
             "a pending delete must not claim success");
}

void RecipeResultFeedbackTest::deleteSuccessShowsDeleted()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipeDeleteResult(true, QString());

    QVERIFY2(page.statusText().contains(QStringLiteral("已删除")),
             qPrintable(QStringLiteral("delete success status was '%1'")
                            .arg(page.statusText())));
}

void RecipeResultFeedbackTest::deleteFailureShowsErrorDetail()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QString detail = QStringLiteral("删除失败: 记录正在被使用");
    page.setRecipeDeleteResult(false, detail);

    QVERIFY2(page.statusText().contains(detail),
             qPrintable(QStringLiteral("delete failure status was '%1'")
                            .arg(page.statusText())));
    QVERIFY(!page.statusText().contains(QStringLiteral("已删除")));
}

void RecipeResultFeedbackTest::deleteFailureDetailSurvivesRepeatedReloads()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    const QString detail = QStringLiteral("删除失败: 数据库忙");
    page.setRecipeDeleteResult(false, detail);

    for (int i = 0; i < 3; ++i) {
        page.setRecipes(oneRecipe());
        QApplication::processEvents();
    }

    QVERIFY2(page.statusText().contains(detail),
             "repeated recipe list reloads discarded the delete failure detail");
}

void RecipeResultFeedbackTest::deleteSuccessThenFailureShowsErrorDetail()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipeDeleteResult(true, QString());
    QVERIFY(page.statusText().contains(QStringLiteral("已删除")));

    const QString detail = QStringLiteral("第二次删除失败: 记录正在被使用");
    page.setRecipeDeleteResult(false, detail);

    QVERIFY2(page.statusText().contains(detail),
             qPrintable(QStringLiteral("delete failure after a success status "
                                       "was '%1'")
                            .arg(page.statusText())));
    QVERIFY(!page.statusText().contains(QStringLiteral("已删除")));
}

// --- OB-6 ---------------------------------------------------------------------

void RecipeResultFeedbackTest::savePendingBlocksDuplicateRequestAndStaysVisible()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.nameEdit()->setText(QStringLiteral("配方A"));

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);

    page.setRecipeSavePending();
    clickAt(page.saveButton());

    QCOMPARE(saveSpy.count(), 0);
    QVERIFY2(!page.statusText().trimmed().isEmpty(),
             "a duplicate save attempt while pending was silent");
}

void RecipeResultFeedbackTest::deletePendingBlocksDuplicateRequestAndStaysVisible()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);

    page.setRecipes(oneRecipe());
    page.recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    QCOMPARE(page.recipeList()->currentRow(), 0);

    QSignalSpy deleteSpy(&page, &RecipeWidthPage::deleteRecipeRequested);

    page.setRecipeDeletePending();
    clickAt(page.deleteButton());

    QCOMPARE(deleteSpy.count(), 0);
    QVERIFY2(!page.statusText().trimmed().isEmpty(),
             "a duplicate delete attempt while pending was silent");
}

void RecipeResultFeedbackTest::saveControlIsUsableAgainAfterResult()
{
    ShellModel model;
    RecipeWidthPage page(model);
    model.setUser(QStringLiteral("admin"), Role::Admin);
    page.nameEdit()->setText(QStringLiteral("配方B"));

    page.setRecipeSavePending();
    page.setRecipeSaveResult(true, QString());

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    clickAt(page.saveButton());

    QCOMPARE(saveSpy.count(), 1);
}

// --- OB-5 / OB-7 ---------------------------------------------------------------

void RecipeResultFeedbackTest::operatorRoleDisablesEditorsWithInlineAdminReason()
{
    ShellModel model;
    model.setUser(QStringLiteral("operator1"), Role::Operator);
    RecipeWidthPage page(model);

    QVERIFY(!page.nameEdit()->isEnabled());
    QVERIFY(!page.widthSpin()->isEnabled());

    QLabel *reason = page.editorReasonLabel();
    QVERIFY2(reason != nullptr, "editorReasonLabel() returned null");
    QVERIFY2(reason->text().contains(QStringLiteral("管理员")),
             qPrintable(QStringLiteral("disabled-editor reason was '%1'")
                            .arg(reason->text())));
    QVERIFY2(!reason->text().trimmed().isEmpty(),
             "disabled editors must show a visible inline reason");
    QVERIFY2(!reason->isHidden(),
             "the disabled-editor reason must not be hidden");

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    QSignalSpy deleteSpy(&page, &RecipeWidthPage::deleteRecipeRequested);
    clickAt(page.saveButton());
    clickAt(page.deleteButton());

    QCOMPARE(saveSpy.count(), 0);
    QCOMPARE(deleteSpy.count(), 0);
}

void RecipeResultFeedbackTest::anonymousRoleDisablesEditorsWithInlineAdminReason()
{
    ShellModel model;
    model.setUser(QString(), Role::Anonymous);
    RecipeWidthPage page(model);

    QVERIFY(!page.nameEdit()->isEnabled());
    QVERIFY(!page.widthSpin()->isEnabled());

    QLabel *reason = page.editorReasonLabel();
    QVERIFY2(reason != nullptr, "editorReasonLabel() returned null");
    QVERIFY2(reason->text().contains(QStringLiteral("管理员")),
             qPrintable(QStringLiteral("disabled-editor reason was '%1'")
                            .arg(reason->text())));
    QVERIFY2(!reason->isHidden(),
             "the disabled-editor reason must not be hidden");

    QSignalSpy saveSpy(&page, &RecipeWidthPage::saveRecipeRequested);
    QSignalSpy deleteSpy(&page, &RecipeWidthPage::deleteRecipeRequested);
    clickAt(page.saveButton());
    clickAt(page.deleteButton());

    QCOMPARE(saveSpy.count(), 0);
    QCOMPARE(deleteSpy.count(), 0);
}

QTEST_MAIN(RecipeResultFeedbackTest)
#include "recipe_result_feedback_test.moc"
