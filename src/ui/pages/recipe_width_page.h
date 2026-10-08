#pragma once

#include <QWidget>
#include <QHash>
#include <QVector>

#include <optional>

#include "ui/pages/recipe_width_model.h"
#include "ui/widgets/width_spin_box.h"

class QLabel;
class QLineEdit;
class QSpinBox;
class QListWidget;
class QVBoxLayout;
class QHideEvent;

namespace hlm {

class ValueDisplay;
class PermissionButton;
class ShellModel;

// 配方与调宽 page (spec §11.3): 配方列表、名称和宽度编辑、D128/D130/D210、
// 调宽状态、应用并调宽.
//
// The page binds RecipeWidthModel to widgets. It never touches Modbus or SQL:
// apply/save/delete intents are emitted as signals for the app shell (Task 20)
// to wire to the ControlCoordinator / DatabaseService. Results arrive via
// ShellModel snapshots (model) or the coordinator's commandResult (fed by the
// shell); the page never shows optimistic success (spec §11.2).
//
// Apply is two-step: the first click arms a "确认应用?" state, the second
// dispatches (spec §10.3: 管理员点击"应用并调宽"并确认后执行).
class RecipeWidthPage : public QWidget
{
    Q_OBJECT

public:
    explicit RecipeWidthPage(ShellModel &model, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    ValueDisplay *fieldDisplay(const QString &key) const;
    QLabel *statusLabel() const;
    QString statusText() const;
    // Inline, touch-visible explanation shown while the recipe name/width
    // editors are disabled (D5: never tooltip-only). Empty for an admin.
    QLabel *editorReasonLabel() const { return m_editorReason; }
    PermissionButton *applyButton() const { return m_apply; }
    PermissionButton *saveButton() const { return m_save; }
    PermissionButton *deleteButton() const { return m_delete; }
    QLineEdit *nameEdit() const { return m_nameEdit; }
    // Raw 0.1 mm units: value() is the register value, the text shows
    // millimetres with one decimal (user decision 2026-09-21).
    WidthSpinBox *widthSpin() const { return m_widthSpin; }
    // 条码个数 editor (user decision 2026-09-23). A plain integer spin box:
    // the value is the count itself, with no 0.1-unit scaling like the width.
    // 0 = do not check.
    QSpinBox *barcodeCountSpin() const { return m_barcodeCountSpin; }
    // The recipe currently loaded in the editors, or nullopt when none is
    // selected. The composition root uses its 条码个数 to judge scan cycles.
    std::optional<RecipeRecord> selectedRecipe() const
    {
        return m_pageModel.selectedRecipe();
    }
    QListWidget *recipeList() const { return m_recipeList; }

    // --- recipe data feed (wired by the app shell, Task 20) --------------------
    void setRecipes(const QVector<RecipeRecord> &recipes);
    // Coordinator result feed (wired by the app shell, Task 20).
    void setAdjustResult(bool ok, const QString &detail);

    // --- page-local database result feed (D2, D3; wired by Task 20) ------------
    // Pending is visible and disables the matching control (D7); results show
    // success text ("已保存"/"已删除") or the failure detail, and survive a
    // later recipe list reload.
    void setRecipeSavePending();
    void setRecipeSaveResult(bool ok, const QString &detail);
    void setRecipeDeletePending();
    void setRecipeDeleteResult(bool ok, const QString &detail);

public slots:
    // Re-renders every widget from the model's current state.
    void refresh();

signals:
    // Write intents for the app shell (Task 20). Never emitted optimistically.
    void applyAdjustRequested(quint16 targetWidth);
    // Raw D128 units (0.1 mm) in and out of the database; the editor shows
    // millimetres with one decimal (user decision 2026-09-21).
    // `barcodeCount` is the recipe's expected barcode count (0 = do not check);
    // it is a plain count, not a scaled register value.
    // `recipeId` is the record to OVERWRITE (-1 = create a new one). Saving
    // under an existing name used to be an unconditional INSERT, which the
    // UNIQUE(name) constraint rejected, so an existing recipe could never be
    // edited (user decision 2026-09-24: 旧配方可以随时更改宽度和扫码个数).
    void saveRecipeRequested(const QString &name, int targetWidthRaw,
                             int barcodeCount, qint64 recipeId);
    void deleteRecipeRequested(qint64 recipeId);
    // The operator picked a different recipe (or cleared the selection). The
    // composition root re-feeds the 条码个数 that judges scan cycles, so a
    // selection change takes effect without waiting for a list reload.
    void recipeSelectionChanged();

protected:
    // Page switch (QStackedWidget hides the page) clears the armed
    // confirmation (spec §11.1-§11.2 页面切换清零意图).
    void hideEvent(QHideEvent *event) override;

private:
    void buildLayout();
    QWidget *addField(const QString &key, const QString &title);
    void onApplyClicked();
    void onRecipeSelected(int row);
    void onSaveClicked();
    void onDeleteClicked();
    // The record a save overwrites, or -1 to create one (see the .cpp).
    qint64 resolveSaveTargetId(const QString &name) const;
    // Resets the two-step confirmation to the idle label.
    void disarmApply();
    // Resets the armed 确认覆盖 state (no-op when none is armed).
    void disarmSaveOverwrite();

    ShellModel &m_model;
    RecipeWidthModel m_pageModel;

    QLabel *m_statusLabel = nullptr;
    QLabel *m_editorReason = nullptr;
    QHash<QString, ValueDisplay *> m_displays;
    QListWidget *m_recipeList = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    WidthSpinBox *m_widthSpin = nullptr;
    QSpinBox *m_barcodeCountSpin = nullptr;
    PermissionButton *m_apply = nullptr;
    PermissionButton *m_save = nullptr;
    PermissionButton *m_delete = nullptr;
    bool m_applyArmed = false;
    // Armed 确认覆盖 state: the recipe id whose overwrite the next 保存配方
    // click will dispatch, or -1 when nothing is armed.
    qint64 m_saveOverwriteId = -1;
};

} // namespace hlm
