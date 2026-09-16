#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

#include "ports/repositories.h" // RecipeRecord

namespace hlm {

class ShellModel;

// 配方与调宽 page model (spec §10.3, §11.3, §11.4). Maps the latest
// ShellModel snapshot + recipe data (fed by the app shell, Task 20) to the
// page's gating state. No I/O, no commands: the page emits the apply/save/
// delete intents; this model only computes permission/interlock gating.
//
// Single adjust-result authority (contract ARCH-017, PLC-HMI-001 D9): the
// displayed terminal adjust verdict comes solely from the coordinator result
// fed through setAdjustResult(); snapshots only render live measurement values
// (D128/D130/D210) and never re-derive or override the verdict.
class RecipeWidthModel : public QObject
{
    Q_OBJECT

public:
    explicit RecipeWidthModel(const ShellModel &model, QObject *parent = nullptr);

    // --- recipe data (fed by the app shell, Task 20) -------------------------
    // Preserves the current selection and the unsaved editor content while the
    // selected record still exists in the reloaded list; clears both only when
    // the selected record disappeared (confirmed deletion, D1/ARCH-004).
    void setRecipes(const QVector<RecipeRecord> &recipes);
    QVector<RecipeRecord> recipes() const { return m_recipes; }
    // 选择配方只把名称和目标宽度加载到界面, 不写 PLC (spec §10.3).
    void selectRecipe(const RecipeRecord &r);
    std::optional<RecipeRecord> selectedRecipe() const { return m_selected; }

    // --- editor state ---------------------------------------------------------
    QString editedName() const { return m_editedName; }
    int editedWidth() const { return m_editedWidth; }
    void setEditedName(const QString &name);
    void setEditedWidth(int width);

    // --- apply gating (permission + interlock + range, spec §10.3 step 1) -----
    bool canApply() const;
    // 配方增删改、应用调宽: 仅管理员 (spec §11.4).
    bool canEditRecipes() const;
    QStringList applyUnmetReasons() const;
    // 目标宽度 == 当前宽度 (D130): 显示"当前已是目标宽度"并结束, 不发 M43
    // (spec §10.3 step 3).
    bool targetEqualsCurrent() const;

    // --- apply lifecycle (spec §10.3) ----------------------------------------
    // Called by the page when applyAdjustRequested is dispatched. Records the
    // applied target and enters the waiting state; the terminal verdict is
    // ONLY fed from the coordinator result via setAdjustResult().
    void beginApply(quint16 targetWidth);
    // Result fed from the coordinator's commandResult (the sole authority).
    void setAdjustResult(bool ok, const QString &detail);

    bool adjustPending() const { return m_adjustPending; }
    bool adjustSucceeded() const { return m_adjustSucceeded; }
    bool adjustFailed() const { return m_adjustFailed; }
    bool adjustResultOk() const { return m_adjustResultOk; }
    std::optional<quint16> appliedTarget() const { return m_appliedTarget; }
    QString adjustStatusText() const;

    // --- page-local save/delete lifecycle (D2, D3) ---------------------------
    // Fed by RecipeWidthPage (dispatch/validation) and Application (database
    // result). These states are page-local only: they are never projected into
    // the shell's machine-command OperatorCommandStatus (contract ARCH-001).
    void setRecipeSavePending();
    void setRecipeSaveResult(bool ok, const QString &detail);
    void setRecipeDeletePending();
    void setRecipeDeleteResult(bool ok, const QString &detail);
    bool recipeSavePending() const { return m_savePending; }
    bool recipeDeletePending() const { return m_deletePending; }
    // Most recent page-local status text: the later of the adjust lifecycle and
    // the recipe save/delete lifecycle. An asynchronous recipe list reload
    // never discards a terminal result (D2/D3, contract invariant 443).
    QString statusText() const;

private:
    const ShellModel &m_model;
    QVector<RecipeRecord> m_recipes;
    std::optional<RecipeRecord> m_selected;
    QString m_editedName;
    int m_editedWidth = 50; // D128 range 50-400 (spec §10.3)

    std::optional<quint16> m_appliedTarget;
    bool m_adjustPending = false;
    bool m_adjustSucceeded = false;
    bool m_adjustFailed = false;
    bool m_resultFed = false;
    bool m_adjustResultOk = false;
    QString m_adjustDetail;

    bool m_savePending = false;
    bool m_deletePending = false;
    QString m_recipeStatusText;
    // Monotonic event order shared by both page-local status sources, so the
    // latest visible event wins without a reload being able to clear it.
    quint64 m_statusEpoch = 0;
    quint64 m_adjustStatusEpoch = 0;
    quint64 m_recipeStatusEpoch = 0;
};

} // namespace hlm
