#include "ui/pages/overview_model.h"

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
    return field(QString::number(m_model.snapshot().targetWidth()),
                 quint8(SnapshotField::TargetWidth)); // D128
}

OverviewField OverviewModel::currentWidth() const
{
    return field(QString::number(m_model.snapshot().currentWidth()),
                 quint8(SnapshotField::CurrentWidth)); // D130
}

OverviewField OverviewModel::widthDelta() const
{
    return field(QString::number(m_model.snapshot().widthDelta()),
                 quint8(SnapshotField::CurrentWidth)); // D210 (int16)
}

OverviewField OverviewModel::beltSpeed() const
{
    return field(QString::number(m_model.snapshot().beltSpeed()),
                 quint8(SnapshotField::BeltSpeed)); // D122
}

OverviewField OverviewModel::productionCount() const
{
    return field(QString::number(m_model.snapshot().productionCount()),
                 quint8(SnapshotField::Heartbeat)); // D138 (no range check)
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

QString OverviewModel::latestAlarmText() const
{
    const QString alarm = m_model.activeAlarmText();
    if (!alarm.isEmpty())
        return alarm;
    return QStringLiteral("无报警");
}

} // namespace hlm