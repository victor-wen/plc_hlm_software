#include "ui/pages/alarm_model.h"

#include "domain/fault_code.h"

namespace hlm {

void AlarmModel::setLoading()
{
    m_state = LoadState::Loading;
    m_failureReason.clear();
}

void AlarmModel::setAlarms(const QVector<AlarmEventRecord> &alarms)
{
    m_all = alarms;
    m_state = LoadState::Loaded;
    m_failureReason.clear();
    refilter();
}

void AlarmModel::setLoadFailed(const QString &reason)
{
    m_state = LoadState::Failed;
    m_failureReason = reason;
    m_all.clear();
    m_filtered.clear();
}

void AlarmModel::setDateRange(const std::optional<QDate> &from,
                              const std::optional<QDate> &to)
{
    m_from = from;
    m_to = to;
    refilter();
}

void AlarmModel::setCodeFilter(const QString &text)
{
    m_codeFilter = text.trimmed();
    refilter();
}

void AlarmModel::refilter()
{
    m_filtered.clear();
    for (const AlarmEventRecord &r : m_all) {
        const QDate started = r.startedAt.date();
        if (m_from && started < *m_from)
            continue;
        if (m_to && started > *m_to)
            continue;
        if (!m_codeFilter.isEmpty()) {
            // PLC: match the code number. HMI (no code): match the
            // messageSnapshot or the source name (spec §11.3 代码筛选).
            const bool codeMatch =
                (r.source == AlarmSource::Plc &&
                 QString::number(r.code) == m_codeFilter) ||
                r.messageSnapshot.contains(m_codeFilter, Qt::CaseInsensitive) ||
                sourceText(r.source).contains(m_codeFilter, Qt::CaseInsensitive);
            if (!codeMatch)
                continue;
        }
        m_filtered.append(r);
    }
}

QString AlarmModel::statusText(const AlarmEventRecord &r)
{
    // endedAt invalid = 活动 (current), valid = 已恢复 (history) (spec §12).
    return r.isActive() ? QStringLiteral("活动") : QStringLiteral("已恢复");
}

QString AlarmModel::sourceText(AlarmSource source)
{
    return source == AlarmSource::Plc ? QStringLiteral("PLC")
                                      : QStringLiteral("HMI");
}

QString AlarmModel::codeText(const AlarmEventRecord &r)
{
    // HMI alarms have no PLC code (spec §12).
    if (r.source == AlarmSource::Hmi)
        return QStringLiteral("—");
    return QString::number(r.code);
}

QString AlarmModel::meaningText(const AlarmEventRecord &r)
{
    if (r.source == AlarmSource::Plc) {
        // PLC 报警代码中文含义 (spec §12): unknown non-zero codes map to the
        // generic text and never crash.
        return FaultCodeTable::instance().info(r.code).meaning;
    }
    return r.messageSnapshot;
}

QString AlarmModel::severityText(AlarmSeverity severity)
{
    switch (severity) {
    case AlarmSeverity::Info:
        return QStringLiteral("提示");
    case AlarmSeverity::Warning:
        return QStringLiteral("警告");
    case AlarmSeverity::Critical:
        return QStringLiteral("严重");
    }
    return QStringLiteral("—");
}

QString AlarmModel::startedText(const AlarmEventRecord &r)
{
    return r.startedAt.isValid()
               ? r.startedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
               : QStringLiteral("—");
}

QString AlarmModel::endedText(const AlarmEventRecord &r)
{
    return r.endedAt.isValid()
               ? r.endedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
               : QStringLiteral("—");
}

QString AlarmModel::statusText() const
{
    switch (m_state) {
    case LoadState::Idle:
        return QStringLiteral("报警数据尚未加载");
    case LoadState::Loading:
        return QStringLiteral("报警数据加载中…");
    case LoadState::Failed:
        return QStringLiteral("报警数据加载失败: %1").arg(m_failureReason);
    case LoadState::Loaded:
        if (m_filtered.isEmpty())
            return QStringLiteral("无报警记录");
        return QStringLiteral("共 %1 条报警记录").arg(m_filtered.size());
    }
    return QString();
}

} // namespace hlm
