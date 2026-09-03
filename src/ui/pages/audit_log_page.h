#pragma once

#include <QDate>
#include <QElapsedTimer>
#include <QWidget>
#include <QVector>

#include "ui/pages/audit_log_model.h"

class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace hlm {

// 操作记录 page (spec §11.3, §12): 时间、用户、角色、动作、对象、脱敏参数、
// 结果和失败原因, 支持时间范围/用户/角色/动作/对象/结果筛选和滚动加载更多.
//
// Strictly read-only: the page declares exactly TWO signals (requestReload and
// requestMore) and contains no command widgets — it can never emit a write
// intent. Audit state comes exclusively from the records fed via setRecords /
// appendRecords (Task 20 wires DatabaseService::recentAuditLoaded); the page
// never derives state from UI actions (spec §11.2: 无乐观状态).
//
// 敏感字段不渲染 (spec §12): the page only displays redactedParameters (已脱敏)
// and never renders passwords/tokens/unredacted content; redaction is the
// application layer's job (Task 8), the page only displays.
//
// 查看操作记录 = 任何人 (spec §11.4): no permission gating on this page.
//
// Async load: the page emits requestReload() when shown (page switch to 操作记录
// requests fresh data) and on the reload button; setLoading/setRecords/
// setLoadFailed drive an explicit status line (加载中 / 加载失败 / 无操作记录).
// 滚动加载: scrolling to the bottom emits requestMore() so the app shell can
// fetch the next page (Task 20); appendRecords() appends without clearing.
// Re-shows within a short window are deduplicated so they do not trigger
// redundant reloads (same pattern as AlarmPage).
class AuditLogPage : public QWidget
{
    Q_OBJECT

public:
    explicit AuditLogPage(QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    AuditLogModel *model() { return &m_model; }
    QTableWidget *table() const { return m_table; }
    QDateEdit *dateFromEdit() const { return m_dateFrom; }
    QDateEdit *dateToEdit() const { return m_dateTo; }
    QLineEdit *actionFilterEdit() const { return m_actionFilter; }
    QLineEdit *userFilterEdit() const { return m_userFilter; }
    QComboBox *roleFilterCombo() const { return m_roleFilter; }
    QLineEdit *targetFilterEdit() const { return m_targetFilter; }
    QComboBox *resultFilterCombo() const { return m_resultFilter; }
    QPushButton *reloadButton() const { return m_reload; }
    QLabel *statusLabel() const { return m_status; }
    QString statusText() const;

public slots:
    // --- async load lifecycle (fed by the app shell, Task 20) ------------------
    void setLoading();
    void setRecords(const QVector<AuditRecord> &records);
    void appendRecords(const QVector<AuditRecord> &records);
    void setLoadFailed(const QString &reason);

signals:
    // The app shell (Task 20) connects these to DatabaseService::listRecentAudit.
    void requestReload();
    void requestMore();

protected:
    // Page switch (QStackedWidget shows the page): request fresh data.
    // Re-shows within kReloadDedupMs of the last reload are ignored (window
    // restore from minimize fires several showEvents in quick succession).
    void showEvent(QShowEvent *event) override;

private:
    void buildLayout();
    void refreshTable();
    void refreshStatus();

    // Re-shows closer than this to the previous reload are deduplicated.
    static constexpr qint64 kReloadDedupMs = 500;

    // Sentinel date shown as "全部" (no date filtering). Any real user-picked
    // date is mapped to the model; the sentinel maps to std::nullopt so the
    // displayed value and the model filter always agree (same pattern as
    // AlarmPage, Task 14).
    static QDate sentinelDate() { return QDate(1900, 1, 1); }

    AuditLogModel m_model;
    QTableWidget *m_table = nullptr;
    QDateEdit *m_dateFrom = nullptr;
    QDateEdit *m_dateTo = nullptr;
    QLineEdit *m_actionFilter = nullptr;
    QLineEdit *m_userFilter = nullptr;
    QComboBox *m_roleFilter = nullptr;
    QLineEdit *m_targetFilter = nullptr;
    QComboBox *m_resultFilter = nullptr;
    QPushButton *m_reload = nullptr;
    QLabel *m_status = nullptr;
    QElapsedTimer m_timer;
    qint64 m_lastReloadMs = 0;
    bool m_moreRequested = false;
};

} // namespace hlm
