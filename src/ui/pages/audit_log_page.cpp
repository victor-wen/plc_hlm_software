#include "ui/pages/audit_log_page.h"

#include <QComboBox>
#include <QDateEdit>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hlm {

namespace {
// Table columns (spec §11.3: 时间、用户、角色、动作、对象、脱敏参数、结果、
// 失败原因).
enum Column {
    ColTime = 0,
    ColUser,
    ColRole,
    ColAction,
    ColTarget,
    ColRedacted,
    ColResult,
    ColReason,
    ColCount
};
} // namespace

AuditLogPage::AuditLogPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("auditLogPage"));
    buildLayout();
    refreshStatus();
}

void AuditLogPage::buildLayout()
{
    // Qt Layout only, no absolute coordinates (spec §11.1). Structure follows
    // spec §11.3: 时间、用户、角色、动作、对象、脱敏参数、结果和失败原因筛选.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // --- filter row: 时间范围 + 动作 + 用户 + 角色 + 对象 + 结果 + 刷新 ----------
    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);

    auto *fromLabel = new QLabel(QStringLiteral("时间从"), this);
    filterRow->addWidget(fromLabel);
    m_dateFrom = new QDateEdit(this);
    m_dateFrom->setObjectName(QStringLiteral("auditDateFrom"));
    m_dateFrom->setCalendarPopup(true);
    m_dateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateFrom->setMinimumHeight(48); // 触摸目标 >= 48 px (spec §11.1)
    // 默认"全部": 哨兵日期显示为"全部", 映射到模型为 nullopt (无日期筛选),
    // 保证显示值与模型筛选状态一致 (same pattern as AlarmPage, Task 14).
    m_dateFrom->setSpecialValueText(QStringLiteral("全部"));
    m_dateFrom->setMinimumDate(sentinelDate());
    m_dateFrom->setDate(sentinelDate());
    filterRow->addWidget(m_dateFrom);

    auto *toLabel = new QLabel(QStringLiteral("至"), this);
    filterRow->addWidget(toLabel);
    m_dateTo = new QDateEdit(this);
    m_dateTo->setObjectName(QStringLiteral("auditDateTo"));
    m_dateTo->setCalendarPopup(true);
    m_dateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateTo->setMinimumHeight(48);
    m_dateTo->setSpecialValueText(QStringLiteral("全部"));
    m_dateTo->setMinimumDate(sentinelDate());
    m_dateTo->setDate(sentinelDate());
    filterRow->addWidget(m_dateTo);

    auto *actionLabel = new QLabel(QStringLiteral("动作"), this);
    filterRow->addWidget(actionLabel);
    m_actionFilter = new QLineEdit(this);
    m_actionFilter->setObjectName(QStringLiteral("auditActionFilter"));
    m_actionFilter->setPlaceholderText(QStringLiteral("动作关键字"));
    m_actionFilter->setMinimumHeight(48); // 触摸目标 >= 48 px (spec §11.1)
    filterRow->addWidget(m_actionFilter, /*stretch=*/1);

    auto *userLabel = new QLabel(QStringLiteral("用户"), this);
    filterRow->addWidget(userLabel);
    m_userFilter = new QLineEdit(this);
    m_userFilter->setObjectName(QStringLiteral("auditUserFilter"));
    m_userFilter->setPlaceholderText(QStringLiteral("用户名"));
    m_userFilter->setMinimumHeight(48);
    filterRow->addWidget(m_userFilter, /*stretch=*/1);

    auto *roleLabel = new QLabel(QStringLiteral("角色"), this);
    filterRow->addWidget(roleLabel);
    m_roleFilter = new QComboBox(this);
    m_roleFilter->setObjectName(QStringLiteral("auditRoleFilter"));
    m_roleFilter->setMinimumHeight(48);
    // 索引 0 = 全部 (无筛选), 1 = 管理员, 2 = 操作员, 3 = 未登录.
    m_roleFilter->addItem(QStringLiteral("全部"));
    m_roleFilter->addItem(QStringLiteral("管理员"));
    m_roleFilter->addItem(QStringLiteral("操作员"));
    m_roleFilter->addItem(QStringLiteral("未登录"));
    filterRow->addWidget(m_roleFilter);

    auto *targetLabel = new QLabel(QStringLiteral("对象"), this);
    filterRow->addWidget(targetLabel);
    m_targetFilter = new QLineEdit(this);
    m_targetFilter->setObjectName(QStringLiteral("auditTargetFilter"));
    m_targetFilter->setPlaceholderText(QStringLiteral("对象关键字"));
    m_targetFilter->setMinimumHeight(48);
    filterRow->addWidget(m_targetFilter, /*stretch=*/1);

    auto *resultLabel = new QLabel(QStringLiteral("结果"), this);
    filterRow->addWidget(resultLabel);
    m_resultFilter = new QComboBox(this);
    m_resultFilter->setObjectName(QStringLiteral("auditResultFilter"));
    m_resultFilter->setMinimumHeight(48);
    // 索引 0 = 全部 (无筛选), 1 = 成功, 2 = 失败.
    m_resultFilter->addItem(QStringLiteral("全部"));
    m_resultFilter->addItem(QStringLiteral("成功"));
    m_resultFilter->addItem(QStringLiteral("失败"));
    filterRow->addWidget(m_resultFilter);

    m_reload = new QPushButton(QStringLiteral("刷新"), this);
    m_reload->setObjectName(QStringLiteral("auditReload"));
    m_reload->setMinimumHeight(48);
    filterRow->addWidget(m_reload);

    root->addLayout(filterRow);

    // --- status line (加载中 / 加载失败 / 无操作记录) -----------------------------
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("auditStatus"));
    m_status->setMinimumHeight(48);
    root->addWidget(m_status);

    // --- audit table --------------------------------------------------------------
    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("auditTable"));
    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("时间"), QStringLiteral("用户"),
        QStringLiteral("角色"), QStringLiteral("动作"),
        QStringLiteral("对象"), QStringLiteral("脱敏参数"),
        QStringLiteral("结果"), QStringLiteral("失败原因"),
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
    // 状态始终一致 (same pattern as AlarmPage, Task 14).
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
    connect(m_actionFilter, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_model.setActionFilter(t);
        refreshTable();
        refreshStatus();
    });
    connect(m_userFilter, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_model.setUserFilter(t);
        refreshTable();
        refreshStatus();
    });
    connect(m_roleFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        // 索引 0 = 全部 (无筛选), 1 = 管理员, 2 = 操作员, 3 = 未登录.
        std::optional<Role> role;
        if (index == 1)
            role = Role::Admin;
        else if (index == 2)
            role = Role::Operator;
        else if (index == 3)
            role = Role::Anonymous;
        m_model.setRoleFilter(role);
        refreshTable();
        refreshStatus();
    });
    connect(m_targetFilter, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_model.setTargetFilter(t);
        refreshTable();
        refreshStatus();
    });
    connect(m_resultFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        // 索引 0 = 全部 (无筛选), 1 = 成功, 2 = 失败.
        std::optional<AuditResult> result;
        if (index == 1)
            result = AuditResult::Success;
        else if (index == 2)
            result = AuditResult::Failure;
        m_model.setResultFilter(result);
        refreshTable();
        refreshStatus();
    });
    connect(m_reload, &QPushButton::clicked, this, &AuditLogPage::requestReload);

    // 滚动到底部: 请求加载更多 (异步分页, Task 20 接 DatabaseService).
    // 已到底部时 valueChanged 会重复触发; m_moreRequested 去重, 直到新数据
    // 到达 (appendRecords/setRecords) 才允许再次请求.
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
                if (value >= m_table->verticalScrollBar()->maximum()
                    && !m_moreRequested) {
                    m_moreRequested = true;
                    emit requestMore();
                }
            });
}

void AuditLogPage::setLoading()
{
    m_model.setLoading();
    refreshTable();
    refreshStatus();
}

void AuditLogPage::setRecords(const QVector<AuditRecord> &records)
{
    m_moreRequested = false; // 新数据到达, 允许再次请求加载更多
    m_model.setRecords(records);
    refreshTable();
    refreshStatus();
}

void AuditLogPage::appendRecords(const QVector<AuditRecord> &records)
{
    m_moreRequested = false; // 新数据到达, 允许再次请求加载更多
    m_model.appendRecords(records);
    refreshTable();
    refreshStatus();
}

void AuditLogPage::setLoadFailed(const QString &reason)
{
    m_model.setLoadFailed(reason);
    refreshTable();
    refreshStatus();
}

void AuditLogPage::refreshTable()
{
    m_table->setRowCount(0);
    m_table->setRowCount(m_model.rowCount());
    for (int row = 0; row < m_model.rowCount(); ++row) {
        const AuditRecord &r = m_model.recordAt(row);
        // 只显示 redactedParameters (已脱敏), 绝不渲染密码/令牌/未脱敏内容
        // (spec §12); 脱敏责任在应用层 (Task 8), 页面只展示.
        auto *time = new QTableWidgetItem(AuditLogModel::timeText(r));
        auto *user = new QTableWidgetItem(AuditLogModel::userText(r));
        auto *role = new QTableWidgetItem(AuditLogModel::roleText(r.role));
        auto *action = new QTableWidgetItem(r.action);
        auto *target = new QTableWidgetItem(r.target);
        auto *redacted = new QTableWidgetItem(r.redactedParameters);
        auto *result = new QTableWidgetItem(AuditLogModel::resultText(r.result));
        auto *reason = new QTableWidgetItem(r.reason);
        m_table->setItem(row, ColTime, time);
        m_table->setItem(row, ColUser, user);
        m_table->setItem(row, ColRole, role);
        m_table->setItem(row, ColAction, action);
        m_table->setItem(row, ColTarget, target);
        m_table->setItem(row, ColRedacted, redacted);
        m_table->setItem(row, ColResult, result);
        m_table->setItem(row, ColReason, reason);
    }
}

void AuditLogPage::refreshStatus()
{
    m_status->setText(m_model.statusText());
}

QString AuditLogPage::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void AuditLogPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Page switch to 操作记录: request fresh data (async load, Task 20 wires it).
    // Window restore from minimize fires several showEvents in quick
    // succession; deduplicate re-shows within a short window so they do not
    // trigger redundant reloads (same pattern as AlarmPage).
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
