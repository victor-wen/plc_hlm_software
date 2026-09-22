#include "ui/pages/overview_model.h"

#include "domain/width_units.h"
#include "ui/shell/shell_model.h"

namespace hlm {

OverviewModel::OverviewModel(const ShellModel &model)
    : m_model(model)
{
}

bool OverviewModel::fresh() const
{
    return m_model.snapshotFresh();
}

OverviewField OverviewModel::field(const QString &text,
                                   quint8 f) const
{
    if (!fresh() || !m_model.snapshot().fieldValid(SnapshotField(f)))
        return {}; // invalid -> "—" (spec §9)
    return {text, true};
}

OverviewField OverviewModel::step() const
{
    if (!fresh())
        return {};
    return {QString::number(m_model.snapshot().currentStep()), true}; // D120
}

OverviewField OverviewModel::targetWidth() const
{
    // D128 is a 0.1 mm register; the operator sees millimetres (user decision
    // 2026-09-21). The snapshot value itself stays raw.
    return field(width_units::rawToDisplay(m_model.snapshot().targetWidth()),
                 quint8(SnapshotField::TargetWidth)); // D128
}

OverviewField OverviewModel::currentWidth() const
{
    return field(width_units::rawToDisplay(m_model.snapshot().currentWidth()),
                 quint8(SnapshotField::CurrentWidth)); // D130
}

OverviewField OverviewModel::widthDelta() const
{
    return field(width_units::rawDeltaToDisplay(m_model.snapshot().widthDelta()),
                 quint8(SnapshotField::CurrentWidth)); // D210 (int16)
}

OverviewField OverviewModel::beltSpeed() const
{
    return field(QString::number(m_model.snapshot().beltSpeed()),
                 quint8(SnapshotField::BeltSpeed)); // D122
}

OverviewField OverviewModel::productionCount() const
{
    // D138 (累计产量) lives in the fast block and is a 32-bit counter with no
    // decoded range, so its validity is the fast block's own freshness: a
    // fresh snapshot whose fast block is Valid. This uses the real per-block
    // quality (PLC-HMI-003 D6) instead of a no-range placeholder field.
    const DeviceSnapshot &s = m_model.snapshot();
    if (!fresh() || s.fastQuality() != DataQuality::Valid)
        return {}; // invalid -> "—" (spec §9)
    return {QString::number(s.productionCount()), true};
}

QString OverviewModel::productionCountReliabilityText() const
{
    return QStringLiteral("自动调宽后该计数可能不可靠（PLC DMUL 覆盖 D138/D139）");
}

bool OverviewModel::online() const
{
    return m_model.online();
}

bool OverviewModel::modeKnown() const
{
    return m_model.modeKnown();
}

bool OverviewModel::isAutoMode() const
{
    return m_model.isAutoMode();
}

bool OverviewModel::isRunning() const
{
    return m_model.isRunning();
}

bool OverviewModel::isFaulted() const
{
    return m_model.isFaulted();
}

bool OverviewModel::m11() const
{
    // Same freshness rule as every other status flag: a stale snapshot must
    // never drive the displayed scan state.
    return m_model.snapshotFresh() && m_model.snapshot().m11();
}

QString OverviewModel::latestAlarmText() const
{
    const QString alarm = m_model.activeAlarmText();
    if (!alarm.isEmpty())
        return alarm;
    return QStringLiteral("无报警");
}

} // namespace hlm