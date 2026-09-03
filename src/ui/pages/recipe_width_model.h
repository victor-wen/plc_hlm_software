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
// page's gating and result state. No I/O, no commands: the page emits the
// apply/save/delete intents; this model only computes permission/interlock
// gating and derives the adjust result from CONFIRMED snapshots (spec §11.2:
// 命令不得乐观更新状态).
//
// The model subscribes to ShellModel::stateChanged and re-evaluates the
// pending adjust result on every snapshot (spec §10.3 steps 5-8):
//   - M34=1: still adjusting, keep waiting.
//   - M34=0 ∧ M44=1 ∧ M45=0 ∧ D130 == applied target: success (step 7).
//   - M34=0 ∧ M44=0 ∧ M45=1: failure (step 8).
//   - Transient idle (all clear): keep waiting.
// Results fed from the coordinator's commandResult (setAdjustResult) are
// shown verbatim and stop snapshot re-evaluation.
class RecipeWidthModel : public QObject
{
    Q_OBJECT

public:
    explicit RecipeWidthModel(const ShellModel &model, QObject *parent = nullptr);

    // --- recipe data (fed by the app shell, Task 20) -------------------------
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
    // applied target and enters the waiting state; the result is derived from
    // later snapshots or fed from the coordinator.
    void beginApply(quint16 targetWidth);
    // Result fed from the coordinator's commandResult (snapshot-confirmed).
    void setAdjustResult(bool ok, const QString &detail);

    bool adjustPending() const { return m_adjustPending; }
    bool adjustSucceeded() const { return m_adjustSucceeded; }
    bool adjustFailed() const { return m_adjustFailed; }
    bool adjustResultOk() const { return m_adjustResultOk; }
    std::optional<quint16> appliedTarget() const { return m_appliedTarget; }
    QString adjustStatusText() const;

private slots:
    // Re-evaluates the pending adjust result from the current snapshot.
    void onShellStateChanged();

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
};

} // namespace hlm
