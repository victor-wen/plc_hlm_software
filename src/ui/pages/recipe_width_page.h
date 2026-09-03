#pragma once

#include <QWidget>
#include <QHash>
#include <QVector>

#include "ui/pages/recipe_width_model.h"

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
    PermissionButton *applyButton() const { return m_apply; }
    PermissionButton *saveButton() const { return m_save; }
    PermissionButton *deleteButton() const { return m_delete; }
    QLineEdit *nameEdit() const { return m_nameEdit; }
    QSpinBox *widthSpin() const { return m_widthSpin; }
    QListWidget *recipeList() const { return m_recipeList; }

    // --- recipe data feed (wired by the app shell, Task 20) --------------------
    void setRecipes(const QVector<RecipeRecord> &recipes);
    // Coordinator result feed (wired by the app shell, Task 20).
    void setAdjustResult(bool ok, const QString &detail);

public slots:
    // Re-renders every widget from the model's current state.
    void refresh();

signals:
    // Write intents for the app shell (Task 20). Never emitted optimistically.
    void applyAdjustRequested(quint16 targetWidth);
    void saveRecipeRequested(const QString &name, int targetWidthMm);
    void deleteRecipeRequested(qint64 recipeId);

protected:
    // Page switch (QStackedWidget hides the page) clears the armed
    // confirmation (spec §11.1-§11.2 页面切换清零意图).
    void hideEvent(QHideEvent *event) override;

private:
    void buildLayout();
    ValueDisplay *addField(const QString &key, const QString &title);
    void onApplyClicked();
    void onRecipeSelected(int row);
    void onSaveClicked();
    void onDeleteClicked();
    // Resets the two-step confirmation to the idle label.
    void disarmApply();

    ShellModel &m_model;
    RecipeWidthModel m_pageModel;

    QLabel *m_statusLabel = nullptr;
    QHash<QString, ValueDisplay *> m_displays;
    QListWidget *m_recipeList = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QSpinBox *m_widthSpin = nullptr;
    PermissionButton *m_apply = nullptr;
    PermissionButton *m_save = nullptr;
    PermissionButton *m_delete = nullptr;
    bool m_applyArmed = false;
};

} // namespace hlm
