#include "ui/pages/recipe_width_model.h"

#include "application/interlock_rules.h"
#include "application/permission_policy.h"
#include "ui/shell/shell_model.h"

namespace hlm {

namespace {

// Permission + interlock reasons for the apply action (spec §10.3 step 1,
// §11.4). Mirrors the shell's ActionBar pattern: permission first, then the
// ordered interlock preconditions.
QStringList applyReasons(const ShellModel &model, int targetWidth)
{
    QStringList reasons;
    const PermissionResult p =
        PermissionPolicy::check(model.role(), Command::AdjustWidth);
    if (!p.allowed && !p.reason.isEmpty())
        reasons.append(p.reason);
    if (!model.snapshotFresh()) {
        reasons.append(QStringLiteral("通讯中断或数据过期"));
        return reasons;
    }
    const InterlockResult il = InterlockRules::checkAdjustWidth(
        model.snapshot(), model.online(), quint16(targetWidth));
    reasons.append(il.unmet);
    return reasons;
}

} // namespace

RecipeWidthModel::RecipeWidthModel(const ShellModel &model, QObject *parent)
    : QObject(parent)
    , m_model(model)
{
    // No snapshot subscription: the terminal adjust verdict is fed solely by
    // the coordinator result (setAdjustResult); snapshots only drive the live
    // D128/D130/D210 rendering in RecipeWidthPage::refresh() (D9/ARCH-017).
}

void RecipeWidthModel::setRecipes(const QVector<RecipeRecord> &recipes)
{
    m_recipes = recipes;
    if (!m_selected.has_value())
        return; // No selection: unsaved editor content is left untouched.

    bool stillPresent = false;
    for (const RecipeRecord &r : recipes) {
        if (r.id == m_selected->id) {
            stillPresent = true;
            break;
        }
    }
    if (stillPresent)
        return; // Keep selection + unsaved edits; fresh record data is not
                // pushed into the editors (D1: the user's draft wins).

    // The selected record is gone (confirmed deletion): drop the stale
    // selection and reset the editors to their neutral state (D1/OB-2).
    m_selected.reset();
    m_editedName.clear();
    m_editedWidth = width_units::kNeutralTargetRaw;
}

void RecipeWidthModel::selectRecipe(const RecipeRecord &r)
{
    // 选择配方只把名称和目标宽度加载到界面, 不写 PLC (spec §10.3).
    m_selected = r;
    m_editedName = r.name;
    m_editedWidth = r.targetWidthRaw;
}

void RecipeWidthModel::setEditedName(const QString &name)
{
    m_editedName = name;
}

void RecipeWidthModel::setEditedWidth(int width)
{
    m_editedWidth = width;
}

bool RecipeWidthModel::canEditRecipes() const
{
    // 配方增删改: 仅管理员 (spec §11.4).
    return PermissionPolicy::check(m_model.role(), Command::AdjustWidth).allowed;
}

bool RecipeWidthModel::canApply() const
{
    return applyReasons(m_model, m_editedWidth).isEmpty();
}

QStringList RecipeWidthModel::applyUnmetReasons() const
{
    return applyReasons(m_model, m_editedWidth);
}

bool RecipeWidthModel::targetEqualsCurrent() const
{
    if (!m_model.snapshotFresh())
        return false;
    const DeviceSnapshot &s = m_model.snapshot();
    return s.fieldValid(SnapshotField::CurrentWidth)
        && s.currentWidth() == quint16(m_editedWidth);
}

void RecipeWidthModel::beginApply(quint16 targetWidth)
{
    m_appliedTarget = targetWidth;
    m_adjustPending = true;
    m_adjustSucceeded = false;
    m_adjustFailed = false;
    m_resultFed = false;
    m_adjustResultOk = false;
    m_adjustDetail.clear();
    m_adjustStatusEpoch = ++m_statusEpoch;
    // The terminal verdict arrives ONLY from the coordinator result
    // (setAdjustResult). Snapshots never derive a terminal state (D9).
}

void RecipeWidthModel::setAdjustResult(bool ok, const QString &detail)
{
    // Result fed from the coordinator (the sole adjust-result authority, D9).
    // Shown verbatim; snapshots can never override it.
    m_resultFed = true;
    m_adjustResultOk = ok;
    m_adjustDetail = detail;
    m_adjustPending = false;
    m_adjustSucceeded = ok;
    m_adjustFailed = !ok;
    m_adjustStatusEpoch = ++m_statusEpoch;
}

void RecipeWidthModel::setRecipeSavePending()
{
    m_savePending = true;
    m_recipeStatusText = QStringLiteral("正在保存配方, 请稍候");
    m_recipeStatusEpoch = ++m_statusEpoch;
}

void RecipeWidthModel::setRecipeSaveResult(bool ok, const QString &detail)
{
    m_savePending = false;
    const QString suffix = detail.trimmed().isEmpty()
                               ? QString()
                               : QStringLiteral(": %1").arg(detail);
    m_recipeStatusText = ok ? QStringLiteral("配方已保存%1").arg(suffix)
                            : QStringLiteral("保存失败%1").arg(suffix);
    m_recipeStatusEpoch = ++m_statusEpoch;
}

void RecipeWidthModel::setRecipeDeletePending()
{
    m_deletePending = true;
    m_recipeStatusText = QStringLiteral("正在删除配方, 请稍候");
    m_recipeStatusEpoch = ++m_statusEpoch;
}

void RecipeWidthModel::setRecipeDeleteResult(bool ok, const QString &detail)
{
    m_deletePending = false;
    const QString suffix = detail.trimmed().isEmpty()
                               ? QString()
                               : QStringLiteral(": %1").arg(detail);
    m_recipeStatusText = ok ? QStringLiteral("配方已删除%1").arg(suffix)
                            : QStringLiteral("删除失败%1").arg(suffix);
    m_recipeStatusEpoch = ++m_statusEpoch;
}

QString RecipeWidthModel::statusText() const
{
    if (m_recipeStatusEpoch > m_adjustStatusEpoch
        && !m_recipeStatusText.isEmpty()) {
        return m_recipeStatusText;
    }
    return adjustStatusText();
}

QString RecipeWidthModel::adjustStatusText() const
{
    if (m_resultFed)
        return m_adjustDetail;
    if (m_adjustPending)
        return QStringLiteral("等待 PLC 结果");
    if (targetEqualsCurrent())
        return QStringLiteral("当前已是目标宽度");
    return QStringLiteral("空闲");
}

} // namespace hlm
