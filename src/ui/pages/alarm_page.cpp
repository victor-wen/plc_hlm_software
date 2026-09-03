#include "ui/pages/alarm_page.h"

#include <QDateEdit>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hlm {

namespace {
// Table columns (spec §11.3: 当前/历史发生/恢复记录 + 日期和代码筛选).
enum Column {
    ColStatus = 0, // 活动 / 已恢复
    ColSource,     // PLC / HMI
    ColCode,       // PLC 故障代码 (HMI: —)
    ColMeaning,    // 代码含义 / HMI messageSnapshot
    ColSeverity,   // 提示 / 警告 / 严重
    ColStarted,    // 发生时间
    ColEnded,      // 恢复时间 (活动: —)
    ColCount
};
} // namespace

AlarmPage::AlarmPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("alarmPage"));
    buildLayout();
    refreshStatus();
}

void AlarmPage::buildLayout()
{
    // Qt Layout only, no absolute coordinates (spec §11.1). Structure follows
    // spec §11.3: 当前 PLC/HMI 报警和历史发生/恢复记录, 支持日期和代码筛选.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // --- filter row: 日期范围 + 代码筛选 + 刷新 ---------------------------------
    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);

    auto *fromLabel = new QLabel(QStringLiteral("发生日期从"), this);
    filterRow->addWidget(fromLabel);
    m_dateFrom = new QDateEdit(this);
    m_dateFrom->setObjectName(QStringLiteral("alarmDateFrom"));
    m_dateFrom->setCalendarPopup(true);
    m_dateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateFrom->setMinimumHeight(48); // 触摸目标 >= 48 px (spec §11.1)
    // 默认"全部": 哨兵日期显示为"全部", 映射到模型为 nullopt (无日期筛选),
    // 保证显示值与模型筛选状态一致 (review finding: 初始日期从未推给模型).
    m_dateFrom->setSpecialValueText(QStringLiteral("全部"));
    m_dateFrom->setMinimumDate(sentinelDate());
    m_dateFrom->setDate(sentinelDate());
    filterRow->addWidget(m_dateFrom);

    auto *toLabel = new QLabel(QStringLiteral("至"), this);
    filterRow->addWidget(toLabel);
    m_dateTo = new QDateEdit(this);
    m_dateTo->setObjectName(QStringLiteral("alarmDateTo"));
    m_dateTo->setCalendarPopup(true);
    m_dateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateTo->setMinimumHeight(48);
    m_dateTo->setSpecialValueText(QStringLiteral("全部"));
    m_dateTo->setMinimumDate(sentinelDate());
    m_dateTo->setDate(sentinelDate());
    filterRow->addWidget(m_dateTo);

    auto *codeLabel = new QLabel(QStringLiteral("代码/内容"), this);
    filterRow->addWidget(codeLabel);
    m_codeFilter = new QLineEdit(this);
    m_codeFilter->setObjectName(QStringLiteral("alarmCodeFilter"));
    m_codeFilter->setPlaceholderText(QStringLiteral("PLC 代码或 HMI 内容"));
    m_codeFilter->setMinimumHeight(48);
    filterRow->addWidget(m_codeFilter, /*stretch=*/1);

    m_reload = new QPushButton(QStringLiteral("刷新"), this);
    m_reload->setObjectName(QStringLiteral("alarmReload"));
    m_reload->setMinimumHeight(48);
    filterRow->addWidget(m_reload);

    root->addLayout(filterRow);

    // --- status line (加载中 / 加载失败 / 无报警记录) -----------------------------
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("alarmStatus"));
    m_status->setMinimumHeight(48);
    root->addWidget(m_status);

    // --- alarm table --------------------------------------------------------------
    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("alarmTable"));
    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("状态"), QStringLiteral("来源"),
        QStringLiteral("代码"), QStringLiteral("含义"),
        QStringLiteral("级别"), QStringLiteral("发生时间"),
        QStringLiteral("恢复时间"),
    });
    // 表格行高不得小于 48 px (spec §11.1).
    m_table->verticalHeader()->setDefaultSectionSize(48);
    m_table->verticalHeader()->setMinimumSectionSize(48);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    root->addWidget(m_table, /*stretch=*/1);

    // --- wiring: filters re-render the table; reload requests fresh data ----------
    // 哨兵日期 (显示"全部") 映射为 nullopt, 真实日期推给模型: 显示值与筛选
    // 状态始终一致 (review finding: 初始日期从未推给模型).
    connect(m_dateFrom, &QDateEdit::dateChanged, this, [this](const QDate &d) {
        const std::optional<QDate> from =
            (d == sentinelDate()) ? std::nullopt : std::optional<QDate>(d);
        m_model.setDateRange(from, m_model.dateTo());
        refreshTable();
        refreshStatus();
    });
    connect(m_dateTo, &QDateEdit::dateChanged, this, [this](const QDate &d) {
        const std::optional<QDate> to =
            (d == sentinelDate()) ? std::nullopt : std::optional<QDate>(d);
        m_model.setDateRange(m_model.dateFrom(), to);
        refreshTable();
        refreshStatus();
    });
    connect(m_codeFilter, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_model.setCodeFilter(t);
        refreshTable();
        refreshStatus();
    });
    connect(m_reload, &QPushButton::clicked, this, &AlarmPage::requestReload);
}

void AlarmPage::setLoading()
{
    m_model.setLoading();
    refreshTable();
    refreshStatus();
}

void AlarmPage::setAlarms(const QVector<AlarmEventRecord> &alarms)
{
    m_model.setAlarms(alarms);
    refreshTable();
    refreshStatus();
}

void AlarmPage::setLoadFailed(const QString &reason)
{
    m_model.setLoadFailed(reason);
    refreshTable();
    refreshStatus();
}

void AlarmPage::refreshTable()
{
    m_table->setRowCount(0);
    m_table->setRowCount(m_model.rowCount());
    for (int row = 0; row < m_model.rowCount(); ++row) {
        const AlarmEventRecord &r = m_model.alarmAt(row);
        auto *status = new QTableWidgetItem(AlarmModel::statusText(r));
        auto *source = new QTableWidgetItem(AlarmModel::sourceText(r.source));
        auto *code = new QTableWidgetItem(AlarmModel::codeText(r));
        auto *meaning = new QTableWidgetItem(AlarmModel::meaningText(r));
        auto *severity = new QTableWidgetItem(AlarmModel::severityText(r.severity));
        auto *started = new QTableWidgetItem(AlarmModel::startedText(r));
        auto *ended = new QTableWidgetItem(AlarmModel::endedText(r));
        m_table->setItem(row, ColStatus, status);
        m_table->setItem(row, ColSource, source);
        m_table->setItem(row, ColCode, code);
        m_table->setItem(row, ColMeaning, meaning);
        m_table->setItem(row, ColSeverity, severity);
        m_table->setItem(row, ColStarted, started);
        m_table->setItem(row, ColEnded, ended);
    }
}

void AlarmPage::refreshStatus()
{
    m_status->setText(m_model.statusText());
}

QString AlarmPage::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void AlarmPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Page switch to 报警: request fresh data (async load, Task 20 wires it).
    // Window restore from minimize fires several showEvents in quick
    // succession; deduplicate re-shows within a short window so they do not
    // trigger redundant reloads (review finding).
    const bool firstShow = !m_timer.isValid();
    if (firstShow)
        m_timer.start();
    const qint64 now = m_timer.elapsed();
    if (firstShow || now - m_lastReloadMs >= kReloadDedupMs) {
        m_lastReloadMs = now;
        emit requestReload();
    }
}

} // namespace hlm
