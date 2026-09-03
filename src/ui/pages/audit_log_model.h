#pragma once

#include <QString>
#include <QVector>

#include <optional>

#include "ports/repositories.h" // AuditRecord, AuditResult, Role

namespace hlm {

// 操作记录 page model (spec §11.3, §12). Pure mapping/state over the audit
// records fed by the app shell (Task 20 wires DatabaseService): no I/O, no
// commands, no SQL. All audit state comes from the passed-in records, never
// derived from UI actions (spec §11.2: 无乐观状态).
//
// - 展示列: 时间、用户、角色、动作、对象、脱敏参数、结果、失败原因
//   (spec §11.3).
// - 匿名用户: username "anonymous" 显示"匿名" (spec §12).
// - 敏感字段: 页面只显示 redactedParameters (已脱敏), 绝不渲染密码/令牌/
//   未脱敏内容; 脱敏责任在应用层 (Task 8), 页面只展示.
// - 筛选: 动作 / 用户 / 结果 (成功/失败) (spec §11.3).
// - 异步分页/滚动: setRecords 替换全部, appendRecords 追加下一页
//   (requestMore 由页面发出, Task 20 接 DatabaseService::listRecentAudit);
//   加载中不阻塞 UI (无阻塞同步调用).
// - 只读: 查看操作记录 = 任何人 (spec §11.4), 页面不发控制命令.
class AuditLogModel
{
public:
    enum class LoadState { Idle, Loading, Loaded, Failed };

    AuditLogModel() = default;

    // --- async load lifecycle (fed by the app shell, Task 20) ----------------
    void setLoading();
    void setRecords(const QVector<AuditRecord> &records);
    void appendRecords(const QVector<AuditRecord> &records);
    void setLoadFailed(const QString &reason);
    LoadState loadState() const { return m_state; }
    QString loadFailureReason() const { return m_failureReason; }

    // --- filters ---------------------------------------------------------------
    void setActionFilter(const QString &text);
    void setUserFilter(const QString &text);
    void setResultFilter(const std::optional<AuditResult> &result);
    QString actionFilter() const { return m_actionFilter; }
    QString userFilter() const { return m_userFilter; }
    std::optional<AuditResult> resultFilter() const { return m_resultFilter; }

    // --- filtered rows -----------------------------------------------------------
    int rowCount() const { return m_filtered.size(); }
    bool isEmpty() const { return m_filtered.isEmpty(); }
    const AuditRecord &recordAt(int row) const { return m_filtered.at(row); }

    // --- display mapping (static, testable without widgets) ----------------------
    static QString timeText(const AuditRecord &r);
    static QString userText(const AuditRecord &r);
    static QString roleText(Role role);
    static QString resultText(AuditResult result);

    // --- status line --------------------------------------------------------------
    QString statusText() const;

private:
    void refilter();

    QVector<AuditRecord> m_all;
    QVector<AuditRecord> m_filtered;
    LoadState m_state = LoadState::Idle;
    QString m_failureReason;
    QString m_actionFilter;
    QString m_userFilter;
    std::optional<AuditResult> m_resultFilter;
};

} // namespace hlm
