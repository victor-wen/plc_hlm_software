#include "ui/pages/manual_control_model.h"

#include "application/interlock_rules.h"
#include "application/permission_policy.h"
#include "ui/shell/shell_model.h"

namespace hlm {

namespace {

// Permission + interlock reasons for the manual commands (spec §10.7, §11.4).
// Mirrors the shell's ActionBar pattern: permission first, then the ordered
// interlock preconditions.
QStringList manualReasons(const ShellModel &model)
{
    QStringList reasons;
    const PermissionResult p =
        PermissionPolicy::check(model.role(), Command::ManualCommand);
    if (!p.allowed && !p.reason.isEmpty())
        reasons.append(p.reason);
    if (!model.snapshotFresh()) {
        reasons.append(QStringLiteral("通讯中断或数据过期"));
        return reasons;
    }
    reasons.append(InterlockRules::checkManualCommand(
                       model.snapshot(), model.online())
                       .unmet);
    return reasons;
}

// Permission + interlock reasons for the bypass commands (spec §10.8, §11.4).
QStringList bypassReasons(const ShellModel &model)
{
    QStringList reasons;
    const PermissionResult p =
        PermissionPolicy::check(model.role(), Command::Bypass);
    if (!p.allowed && !p.reason.isEmpty())
        reasons.append(p.reason);
    if (!model.snapshotFresh()) {
        reasons.append(QStringLiteral("通讯中断或数据过期"));
        return reasons;
    }
    reasons.append(InterlockRules::checkBypass(model.snapshot(), model.online())
                       .unmet);
    return reasons;
}

} // namespace

ManualControlModel::ManualControlModel(const ShellModel &model, QObject *parent)
    : QObject(parent)
    , m_model(model)
{
    connect(&m_model, &ShellModel::stateChanged, this,
            &ManualControlModel::onShellStateChanged);
}

bool ManualControlModel::canManual() const
{
    return manualReasons(m_model).isEmpty();
}

QStringList ManualControlModel::manualUnmetReasons() const
{
    return manualReasons(m_model);
}

bool ManualControlModel::canBypass() const
{
    return bypassReasons(m_model).isEmpty();
}

QStringList ManualControlModel::bypassUnmetReasons() const
{
    return bypassReasons(m_model);
}

void ManualControlModel::armShield(quint16 address)
{
    // Only M110/M111 are safety shields (spec §10.8).
    if (address != 110 && address != 111)
        return;
    m_shieldArmed = address;
    // The armed target is derived from the CURRENT readback: 屏蔽 when the
    // shield is inactive, 恢复 when it is active. The confirmation is only
    // valid while the readback still matches (checked on dispatch).
    m_shieldTarget = (address == 110) ? !m110() : !m111();
}

void ManualControlModel::disarmShield(quint16 address)
{
    if (m_shieldArmed == address) {
        m_shieldArmed.reset();
        m_shieldTarget.reset();
    }
}

void ManualControlModel::disarmAllShields()
{
    m_shieldArmed.reset();
    m_shieldTarget.reset();
}

bool ManualControlModel::shieldArmed(quint16 address) const
{
    return m_shieldArmed == address;
}

std::optional<bool> ManualControlModel::shieldTarget(quint16 address) const
{
    if (m_shieldArmed != address)
        return std::nullopt;
    return m_shieldTarget;
}

bool ManualControlModel::m42() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m42();
}

bool ManualControlModel::m105() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m105();
}

bool ManualControlModel::m109() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m109();
}

bool ManualControlModel::m110() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m110();
}

bool ManualControlModel::m111() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m111();
}

bool ManualControlModel::m106() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m106();
}

bool ManualControlModel::m107() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m107();
}

bool ManualControlModel::m108() const
{
    return m_model.snapshotFresh() && m_model.snapshot().m108();
}

bool ManualControlModel::shieldActive() const
{
    return m110() || m111();
}

bool ManualControlModel::widthSpeedValid() const
{
    return m_model.snapshotFresh()
        && m_model.snapshot().fieldValid(SnapshotField::WidthSpeed);
}

quint16 ManualControlModel::widthSpeed() const
{
    return m_model.snapshot().widthSpeed();
}

QString ManualControlModel::statusText() const
{
    if (shieldActive())
        return QStringLiteral("安全屏蔽生效");
    if (m109())
        return QStringLiteral("挡停伸出");
    if (m106() || m107() || m108())
        return QStringLiteral("手动命令执行中");
    return QStringLiteral("空闲");
}

void ManualControlModel::onShellStateChanged()
{
    // A gate change that closes the bypass gate (e.g. going offline) must
    // reset the armed shield confirmation: the next click would dispatch
    // without a fresh confirm (spec §10.8 二次确认, §11.1-§11.2 门控变化清零).
    if (m_shieldArmed.has_value() && !canBypass())
        disarmAllShields();
}

} // namespace hlm
