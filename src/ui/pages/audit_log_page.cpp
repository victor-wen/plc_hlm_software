#include "ui/pages/audit_log_page.h"

#include <QComboBox>
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

    // --- filter row: 动作 + 用户 + 结果 + 刷新 -----------------------------------
    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(8);

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
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
                if (value >= m_table->verticalScrollBar()->maximum())
                    emit requestMore();
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
    m_model.setRecords(records);
    refreshTable();
    refreshStatus();
}

void AuditLogPage::appendRecords(const QVector<AuditRecord> &records)
{
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
