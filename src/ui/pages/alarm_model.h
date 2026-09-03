#pragma once

#include <QDate>
#include <QString>
#include <QVector>

#include <optional>

#include "ports/repositories.h" // AlarmEventRecord, AlarmSource

namespace hlm {

// 报警 page model (spec §11.3, §12). Pure mapping/state over the alarm
// records fed by the app shell (Task 20 wires DatabaseService): no I/O, no
// commands, no SQL. All alarm state comes from the passed-in records
// (snapshot/database), never derived from UI actions (spec §11.2: 无乐观状态).
//
// - 活动/恢复: endedAt invalid = 活动 (current), valid = 已恢复 (history).
// - PLC 代码含义 via FaultCodeTable::instance().info(code); unknown non-zero
//   codes show the generic text and never crash (spec §12).
// - HMI alarms have no PLC code: code column "—", meaning from
//   messageSnapshot (spec §12: HMI 系统报警至少包含通讯中断等).
// - 筛选: date range on startedAt; code filter matches the PLC code number,
//   or for HMI records the messageSnapshot / source name.
// - 无确认语义: no confirm/acknowledge state (spec §12).
class AlarmModel
{
public:
    enum class LoadState { Idle, Loading, Loaded, Failed };

    AlarmModel() = default;

    // --- async load lifecycle (fed by the app shell, Task 20) ----------------
    void setLoading();
    void setAlarms(const QVector<AlarmEventRecord> &alarms);
    void setLoadFailed(const QString &reason);
    LoadState loadState() const { return m_state; }
    QString loadFailureReason() const { return m_failureReason; }

    // --- filters ---------------------------------------------------------------
    void setDateRange(const std::optional<QDate> &from,
                      const std::optional<QDate> &to);
    void setCodeFilter(const QString &text);
    std::optional<QDate> dateFrom() const { return m_from; }
    std::optional<QDate> dateTo() const { return m_to; }
    QString codeFilter() const { return m_codeFilter; }

    // --- filtered rows -----------------------------------------------------------
    int rowCount() const { return m_filtered.size(); }
    bool isEmpty() const { return m_filtered.isEmpty(); }
    const AlarmEventRecord &alarmAt(int row) const { return m_filtered.at(row); }

    // --- display mapping (static, testable without widgets) ----------------------
    static QString statusText(const AlarmEventRecord &r);
    static QString sourceText(AlarmSource source);
    static QString codeText(const AlarmEventRecord &r);
    static QString meaningText(const AlarmEventRecord &r);
    static QString severityText(AlarmSeverity severity);
    static QString startedText(const AlarmEventRecord &r);
    static QString endedText(const AlarmEventRecord &r);

    // --- status line --------------------------------------------------------------
    QString statusText() const;

private:
    void refilter();

    QVector<AlarmEventRecord> m_all;
    QVector<AlarmEventRecord> m_filtered;
    LoadState m_state = LoadState::Idle;
    QString m_failureReason;
    std::optional<QDate> m_from;
    std::optional<QDate> m_to;
    bool m_invalidRange = false;
    QString m_codeFilter;
};

} // namespace hlm
