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
    connect(&m_model, &ShellModel::stateChanged, this,
            &RecipeWidthModel::onShellStateChanged);
}

void RecipeWidthModel::setRecipes(const QVector<RecipeRecord> &recipes)
{
    m_recipes = recipes;
}

void RecipeWidthModel::selectRecipe(const RecipeRecord &r)
{
    // 选择配方只把名称和目标宽度加载到界面, 不写 PLC (spec §10.3).
    m_selected = r;
    m_editedName = r.name;
    m_editedWidth = r.targetWidthMm;
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
    // The result is derived from LATER snapshots (spec §10.3 steps 5-8) or
    // fed from the coordinator's commandResult. The page pre-checks
    // targetEqualsCurrent() and the coordinator gates on M34=0, so the
    // current snapshot can never already be this command's result.
}

void RecipeWidthModel::setAdjustResult(bool ok, const QString &detail)
{
    // Result fed from the coordinator (snapshot-confirmed upstream, spec
    // §11.2). Shown verbatim; stops snapshot re-evaluation.
    m_resultFed = true;
    m_adjustResultOk = ok;
    m_adjustDetail = detail;
    m_adjustPending = false;
    m_adjustSucceeded = ok;
    m_adjustFailed = !ok;
}

void RecipeWidthModel::onShellStateChanged()
{
    if (!m_adjustPending || m_resultFed)
        return;
    if (!m_model.snapshotFresh())
        return; // keep waiting; offline/stale handled by the coordinator
    const DeviceSnapshot &s = m_model.snapshot();
    if (s.m34())
        return; // still adjusting (spec §10.3 step 6)
    // Success: M34=0, M44=1, M45=0, D130 == applied target (step 7).
    if (s.m44() && !s.m45()
        && s.fieldValid(SnapshotField::CurrentWidth)
        && s.currentWidth() == m_appliedTarget.value_or(0)) {
        m_adjustPending = false;
        m_adjustSucceeded = true;
        m_adjustFailed = false;
        m_adjustDetail = QStringLiteral("调宽成功");
        return;
    }
    // Failure: M34=0, M44=0, M45=1 (step 8).
    if (!s.m44() && s.m45()) {
        m_adjustPending = false;
        m_adjustSucceeded = false;
        m_adjustFailed = true;
        m_adjustDetail = QStringLiteral("调宽失败");
        return;
    }
    // Transient idle (M34=0, M44=0, M45=0): keep waiting for the PLC result.
}

QString RecipeWidthModel::adjustStatusText() const
{
    if (m_resultFed)
        return m_adjustDetail;
    if (m_adjustSucceeded)
        return QStringLiteral("调宽成功");
    if (m_adjustFailed)
        return QStringLiteral("调宽失败");
    if (m_adjustPending)
        return QStringLiteral("等待 PLC 结果");
    if (targetEqualsCurrent())
        return QStringLiteral("当前已是目标宽度");
    return QStringLiteral("空闲");
}

} // namespace hlm
