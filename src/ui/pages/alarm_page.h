#pragma once

#include <QWidget>
#include <QVector>

#include "ui/pages/alarm_model.h"

class QLabel;
class QLineEdit;
class QDateEdit;
class QPushButton;
class QTableWidget;

namespace hlm {

// 报警 page (spec §11.3, §12): 当前 PLC/HMI 报警和历史发生/恢复记录, 支持
// 日期和代码筛选.
//
// Strictly read-only: the page declares exactly ONE signal (requestReload) and
// contains no command widgets — it can never emit a write intent. Alarm state
// comes exclusively from the records fed via setAlarms (Task 20 wires
// DatabaseService::recentAlarmsLoaded); the page never derives state from UI
// actions (spec §11.2: 无乐观状态).
//
// 无确认语义 (spec §12): no confirm/acknowledge control or state exists.
// 查看报警 = 任何人 (spec §11.4): no permission gating on this page.
//
// Async load: the page emits requestReload() when shown and on the reload
// button; setLoading/setAlarms/setLoadFailed drive an explicit status line
// (加载中 / 加载失败 / 无报警记录).
class AlarmPage : public QWidget
{
    Q_OBJECT

public:
    explicit AlarmPage(QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    AlarmModel *model() { return &m_model; }
    QTableWidget *table() const { return m_table; }
    QDateEdit *dateFromEdit() const { return m_dateFrom; }
    QDateEdit *dateToEdit() const { return m_dateTo; }
    QLineEdit *codeFilterEdit() const { return m_codeFilter; }
    QPushButton *reloadButton() const { return m_reload; }
    QLabel *statusLabel() const { return m_status; }
    QString statusText() const;

public slots:
    // --- async load lifecycle (fed by the app shell, Task 20) ------------------
    void setLoading();
    void setAlarms(const QVector<AlarmEventRecord> &alarms);
    void setLoadFailed(const QString &reason);

signals:
    // The app shell (Task 20) connects this to DatabaseService::listRecentAlarms.
    void requestReload();

protected:
    // Page switch (QStackedWidget shows the page): request fresh data.
    void showEvent(QShowEvent *event) override;

private:
    void buildLayout();
    void refreshTable();
    void refreshStatus();

    AlarmModel m_model;
    QTableWidget *m_table = nullptr;
    QDateEdit *m_dateFrom = nullptr;
    QDateEdit *m_dateTo = nullptr;
    QLineEdit *m_codeFilter = nullptr;
    QPushButton *m_reload = nullptr;
    QLabel *m_status = nullptr;
};

} // namespace hlm
