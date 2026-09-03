#include "ui/pages/audit_log_model.h"

namespace hlm {

void AuditLogModel::setLoading()
{
    m_state = LoadState::Loading;
    m_failureReason.clear();
}

void AuditLogModel::setRecords(const QVector<AuditRecord> &records)
{
    m_all = records;
    m_state = LoadState::Loaded;
    m_failureReason.clear();
    refilter();
}

void AuditLogModel::appendRecords(const QVector<AuditRecord> &records)
{
    // 滚动加载: 追加下一页, 不清空已有记录 (spec §11.3 异步分页/滚动).
    m_all.append(records);
    m_state = LoadState::Loaded;
    m_failureReason.clear();
    refilter();
}

void AuditLogModel::setLoadFailed(const QString &reason)
{
    m_state = LoadState::Failed;
    m_failureReason = reason;
    m_all.clear();
    m_filtered.clear();
}

void AuditLogModel::setDateRange(const std::optional<QDate> &from,
                                 const std::optional<QDate> &to)
{
    m_from = from;
    m_to = to;
    refilter();
}

void AuditLogModel::setActionFilter(const QString &text)
{
    m_actionFilter = text.trimmed();
    refilter();
}

void AuditLogModel::setUserFilter(const QString &text)
{
    m_userFilter = text.trimmed();
    refilter();
}

void AuditLogModel::setRoleFilter(const std::optional<Role> &role)
{
    m_roleFilter = role;
    refilter();
}

void AuditLogModel::setTargetFilter(const QString &text)
{
    m_targetFilter = text.trimmed();
    refilter();
}

void AuditLogModel::setResultFilter(const std::optional<AuditResult> &result)
{
    m_resultFilter = result;
    refilter();
}

void AuditLogModel::refilter()
{
    m_filtered.clear();
    for (const AuditRecord &r : m_all) {
        // 时间范围: 按 occurredAt 的日期筛选 (spec §11.3 时间筛选).
        const QDate occurred = r.occurredAt.date();
        if (m_from && occurred < *m_from)
            continue;
        if (m_to && occurred > *m_to)
            continue;
        if (!m_actionFilter.isEmpty()
            && !r.action.contains(m_actionFilter, Qt::CaseInsensitive))
            continue;
        if (!m_userFilter.isEmpty()
            && !r.username.contains(m_userFilter, Qt::CaseInsensitive))
            continue;
        if (m_roleFilter && r.role != *m_roleFilter)
            continue;
        if (!m_targetFilter.isEmpty()
            && !r.target.contains(m_targetFilter, Qt::CaseInsensitive))
            continue;
        if (m_resultFilter && r.result != *m_resultFilter)
            continue;
        m_filtered.append(r);
    }
}

QString AuditLogModel::timeText(const AuditRecord &r)
{
    return r.occurredAt.isValid()
               ? r.occurredAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
               : QStringLiteral("—");
}

QString AuditLogModel::userText(const AuditRecord &r)
{
    // spec §12: username "anonymous" 显示"匿名".
    return r.username == QStringLiteral("anonymous") ? QStringLiteral("匿名")
                                                     : r.username;
}

QString AuditLogModel::roleText(Role role)
{
    switch (role) {
    case Role::Anonymous:
        return QStringLiteral("未登录");
    case Role::Operator:
        return QStringLiteral("操作员");
    case Role::Admin:
        return QStringLiteral("管理员");
    }
    return QStringLiteral("—");
}

QString AuditLogModel::resultText(AuditResult result)
{
    return result == AuditResult::Success ? QStringLiteral("成功")
                                          : QStringLiteral("失败");
}

QString AuditLogModel::statusText() const
{
    switch (m_state) {
    case LoadState::Idle:
        return QStringLiteral("操作记录尚未加载");
    case LoadState::Loading:
        return QStringLiteral("操作记录加载中…");
    case LoadState::Failed:
        return QStringLiteral("操作记录加载失败: %1").arg(m_failureReason);
    case LoadState::Loaded:
        if (m_filtered.isEmpty()) {
            const bool filtersActive = m_from.has_value() || m_to.has_value()
                                       || !m_actionFilter.isEmpty()
                                       || !m_userFilter.isEmpty()
                                       || m_roleFilter.has_value()
                                       || !m_targetFilter.isEmpty()
                                       || m_resultFilter.has_value();
            return filtersActive ? QStringLiteral("无匹配记录")
                                 : QStringLiteral("无操作记录");
        }
        return QStringLiteral("共 %1 条操作记录").arg(m_filtered.size());
    }
    return QString();
}

} // namespace hlm
