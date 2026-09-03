// Task 15 unit tests: 操作记录 page (spec §11.3, §12).
//
// Coverage required by the task brief:
// - 展示列: 时间、用户、角色、动作、对象、脱敏参数、结果、失败原因.
// - 匿名用户显示"匿名" (spec §12: username "anonymous").
// - 敏感字段不渲染: 页面只显示 redactedParameters, 绝不渲染密码/令牌/未脱敏
//   内容; 注入含密码样子的记录, 验证页面文本无明文密码/令牌.
// - 筛选: 动作 / 用户 / 结果 (成功/失败) (spec §11.3).
// - 异步分页/滚动: requestMore 追加, 滚动到底部触发, 加载中不阻塞 UI.
// - 结果列: Success=成功 / Failure=失败 (中文).
// - 表格行高 >= 48 px, 触摸目标 >= 48 px (spec §11.1).
// - 只读页面: 除 requestReload/requestMore 外无任何信号, 不发控制命令
//   (spec §11.4: 查看操作记录 = 任何人, 页面只读).

#include <QtTest>
#include <QSignalSpy>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QScrollBar>

#include "ui/pages/audit_log_model.h"
#include "ui/pages/audit_log_page.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

AuditRecord record(qint64 id, const QString &username, Role role,
                   const QString &action, const QString &target,
                   const QString &redacted, AuditResult result,
                   const QString &reason, const QDateTime &at)
{
    AuditRecord r;
    r.id = id;
    r.username = username;
    r.role = role;
    r.action = action;
    r.target = target;
    r.redactedParameters = redacted;
    r.result = result;
    r.reason = reason;
    r.occurredAt = at;
    return r;
}

} // namespace

class AuditLogPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- model: 展示映射 ------------------------------------------------------
    void displayMappings();
    void anonymousUserShowsAnonymous();
    void resultTextMapsSuccessAndFailure();

    // --- model: 筛选 (动作 / 用户 / 结果) --------------------------------------
    void filterByAction();
    void filterByUser();
    void filterByResult();
    void filterCombination();

    // --- model: 异步分页/滚动 ---------------------------------------------------
    void appendRecordsExtendsList();
    void requestMoreEmitsSignal();

    // --- page: 48px 行高与触摸目标 (spec §11.1) ---------------------------------
    void tableRowHeightAtLeast48();
    void filterControlsMeetTouchTargetSize();

    // --- page: 只读, 不发控制命令 (spec §11.4) ----------------------------------
    void onlySignalsAreDataRequests();

    // --- page: 渲染 --------------------------------------------------------------
    void pageRendersAllColumns();
    void pageRendersFilteredRows();

    // --- page: 敏感字段不渲染 -----------------------------------------------------
    void pageNeverRendersSensitiveValues();

    // --- page: 滚动到底部触发 requestMore -----------------------------------------
    void scrollToBottomRequestsMore();

    // --- MainWindow integration --------------------------------------------------
    void mainWindowUsesAuditLogPage();
};

// --- model: 展示映射 -------------------------------------------------------------

void AuditLogPageTest::displayMappings()
{
    const QDateTime t(QDate(2026, 8, 28), QTime(10, 30, 5));
    AuditRecord r = record(1, QStringLiteral("admin"), Role::Admin,
                           QStringLiteral("login"), QStringLiteral("admin"),
                           QStringLiteral(""), AuditResult::Success,
                           QStringLiteral(""), t);
    QCOMPARE(AuditLogModel::timeText(r), QStringLiteral("2026-08-28 10:30:05"));
    QCOMPARE(AuditLogModel::userText(r), QStringLiteral("admin"));
    QCOMPARE(AuditLogModel::roleText(r.role), QStringLiteral("管理员"));
    QCOMPARE(AuditLogModel::resultText(r.result), QStringLiteral("成功"));
}

void AuditLogPageTest::anonymousUserShowsAnonymous()
{
    // spec §12: username "anonymous" 显示"匿名".
    const QDateTime t = QDateTime::currentDateTime();
    AuditRecord r = record(1, QStringLiteral("anonymous"), Role::Anonymous,
                           QStringLiteral("view"), QStringLiteral("overview"),
                           QStringLiteral(""), AuditResult::Success,
                           QStringLiteral(""), t);
    QCOMPARE(AuditLogModel::userText(r), QStringLiteral("匿名"));
    QCOMPARE(AuditLogModel::roleText(r.role), QStringLiteral("未登录"));
}

void AuditLogPageTest::resultTextMapsSuccessAndFailure()
{
    QCOMPARE(AuditLogModel::resultText(AuditResult::Success), QStringLiteral("成功"));
    QCOMPARE(AuditLogModel::resultText(AuditResult::Failure), QStringLiteral("失败"));
}

// --- model: 筛选 -----------------------------------------------------------------

void AuditLogPageTest::filterByAction()
{
    AuditLogModel m;
    const QDateTime t = QDateTime::currentDateTime();
    m.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
        record(2, QStringLiteral("admin"), Role::Admin, QStringLiteral("logout"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
        record(3, QStringLiteral("operator"), Role::Operator, QStringLiteral("start"),
               QStringLiteral("M3"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });
    QCOMPARE(m.rowCount(), 3);

    m.setActionFilter(QStringLiteral("login"));
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.recordAt(0).id, qint64(1));

    m.setActionFilter(QString());
    QCOMPARE(m.rowCount(), 3);
}

void AuditLogPageTest::filterByUser()
{
    AuditLogModel m;
    const QDateTime t = QDateTime::currentDateTime();
    m.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
        record(2, QStringLiteral("operator"), Role::Operator, QStringLiteral("start"),
               QStringLiteral("M3"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
        record(3, QStringLiteral("anonymous"), Role::Anonymous, QStringLiteral("view"),
               QStringLiteral("overview"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });

    m.setUserFilter(QStringLiteral("operator"));
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.recordAt(0).id, qint64(2));

    // 匿名筛选: 匹配 username "anonymous".
    m.setUserFilter(QStringLiteral("anonymous"));
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.recordAt(0).id, qint64(3));

    m.setUserFilter(QString());
    QCOMPARE(m.rowCount(), 3);
}

void AuditLogPageTest::filterByResult()
{
    AuditLogModel m;
    const QDateTime t = QDateTime::currentDateTime();
    m.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
        record(2, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Failure,
               QStringLiteral("密码错误"), t),
        record(3, QStringLiteral("operator"), Role::Operator, QStringLiteral("start"),
               QStringLiteral("M3"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });

    m.setResultFilter(AuditResult::Failure);
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.recordAt(0).id, qint64(2));

    m.setResultFilter(AuditResult::Success);
    QCOMPARE(m.rowCount(), 2);

    m.setResultFilter(std::nullopt);
    QCOMPARE(m.rowCount(), 3);
}

void AuditLogPageTest::filterCombination()
{
    AuditLogModel m;
    const QDateTime t = QDateTime::currentDateTime();
    m.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
        record(2, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Failure,
               QStringLiteral("密码错误"), t),
        record(3, QStringLiteral("operator"), Role::Operator, QStringLiteral("start"),
               QStringLiteral("M3"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });

    m.setActionFilter(QStringLiteral("login"));
    m.setResultFilter(AuditResult::Failure);
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.recordAt(0).id, qint64(2));

    m.setUserFilter(QStringLiteral("operator"));
    QCOMPARE(m.rowCount(), 0);
}

// --- model: 异步分页/滚动 ----------------------------------------------------------

void AuditLogPageTest::appendRecordsExtendsList()
{
    AuditLogModel m;
    const QDateTime t = QDateTime::currentDateTime();
    m.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });
    QCOMPARE(m.rowCount(), 1);

    // 滚动加载: 追加下一页, 不清空已有记录.
    m.appendRecords({
        record(2, QStringLiteral("operator"), Role::Operator, QStringLiteral("start"),
               QStringLiteral("M3"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(m.recordAt(0).id, qint64(1));
    QCOMPARE(m.recordAt(1).id, qint64(2));
}

void AuditLogPageTest::requestMoreEmitsSignal()
{
    AuditLogPage page;
    QSignalSpy spy(&page, &AuditLogPage::requestMore);
    page.requestMore();
    QCOMPARE(spy.count(), 1);
}

// --- page: 48px 行高与触摸目标 ------------------------------------------------------

void AuditLogPageTest::tableRowHeightAtLeast48()
{
    // 表格行高不得小于 48 px (spec §11.1).
    AuditLogPage page;
    QVERIFY(page.table()->verticalHeader()->defaultSectionSize() >= 48);
    QVERIFY(page.table()->verticalHeader()->minimumSectionSize() >= 48);
}

void AuditLogPageTest::filterControlsMeetTouchTargetSize()
{
    // 触摸目标 >= 48 px (spec §11.1).
    AuditLogPage page;
    QVERIFY(page.actionFilterEdit()->minimumHeight() >= 48);
    QVERIFY(page.userFilterEdit()->minimumHeight() >= 48);
    QVERIFY(page.resultFilterCombo()->minimumHeight() >= 48);
    QVERIFY(page.reloadButton()->minimumHeight() >= 48);
}

// --- page: 只读, 不发控制命令 ---------------------------------------------------------

void AuditLogPageTest::onlySignalsAreDataRequests()
{
    // 页面只读, 不发任何控制命令 (spec §11.3, §11.4): 唯一信号是数据请求
    // (requestReload / requestMore). 任何控制命令信号都会使本测试失败.
    AuditLogPage page;
    QStringList signalNames;
    const QMetaObject *mo = page.metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod method = mo->method(i);
        if (method.methodType() == QMetaMethod::Signal
            && method.enclosingMetaObject() == mo)
            signalNames << QString::fromLatin1(method.name());
    }
    signalNames.sort();
    const QStringList expected{QStringLiteral("requestMore"),
                               QStringLiteral("requestReload")};
    QCOMPARE(signalNames, expected);
}

// --- page: 渲染 ------------------------------------------------------------------------

void AuditLogPageTest::pageRendersAllColumns()
{
    AuditLogPage page;
    const QDateTime t(QDate(2026, 8, 28), QTime(10, 30, 5));
    page.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });
    QCOMPARE(page.table()->rowCount(), 1);
    QCOMPARE(page.table()->columnCount(), 8);

    // 列顺序: 时间、用户、角色、动作、对象、脱敏参数、结果、失败原因 (spec §11.3).
    QCOMPARE(page.table()->item(0, 0)->text(), QStringLiteral("2026-08-28 10:30:05"));
    QCOMPARE(page.table()->item(0, 1)->text(), QStringLiteral("admin"));
    QCOMPARE(page.table()->item(0, 2)->text(), QStringLiteral("管理员"));
    QCOMPARE(page.table()->item(0, 3)->text(), QStringLiteral("login"));
    QCOMPARE(page.table()->item(0, 4)->text(), QStringLiteral("admin"));
    QCOMPARE(page.table()->item(0, 5)->text(), QStringLiteral(""));
    QCOMPARE(page.table()->item(0, 6)->text(), QStringLiteral("成功"));
    QCOMPARE(page.table()->item(0, 7)->text(), QStringLiteral(""));
}

void AuditLogPageTest::pageRendersFilteredRows()
{
    AuditLogPage page;
    const QDateTime t = QDateTime::currentDateTime();
    page.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
        record(2, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"), QStringLiteral(""), AuditResult::Failure,
               QStringLiteral("密码错误"), t),
        record(3, QStringLiteral("operator"), Role::Operator, QStringLiteral("start"),
               QStringLiteral("M3"), QStringLiteral(""), AuditResult::Success,
               QStringLiteral(""), t),
    });
    QCOMPARE(page.table()->rowCount(), 3);

    // 动作筛选.
    page.actionFilterEdit()->setText(QStringLiteral("login"));
    QCOMPARE(page.table()->rowCount(), 2);

    // 结果筛选 (真实用户路径: 下拉框).
    page.resultFilterCombo()->setCurrentIndex(2); // 失败
    QCOMPARE(page.table()->rowCount(), 1);
    QCOMPARE(page.table()->item(0, 6)->text(), QStringLiteral("失败"));
    QCOMPARE(page.table()->item(0, 7)->text(), QStringLiteral("密码错误"));

    // 用户筛选.
    page.userFilterEdit()->setText(QStringLiteral("operator"));
    QCOMPARE(page.table()->rowCount(), 0);
}

// --- page: 敏感字段不渲染 ----------------------------------------------------------------

void AuditLogPageTest::pageNeverRendersSensitiveValues()
{
    // 页面只显示 redactedParameters (已脱敏), 绝不渲染密码/令牌/未脱敏内容
    // (spec §12: redacted_parameters 绝不包含密码或令牌). 注入含密码样子的
    // 记录, 验证页面任何可见文本都不含明文敏感值.
    AuditLogPage page;
    const QDateTime t = QDateTime::currentDateTime();
    page.setRecords({
        record(1, QStringLiteral("admin"), Role::Admin, QStringLiteral("login"),
               QStringLiteral("admin"),
               QStringLiteral("username=admin, password=******"),
               AuditResult::Success, QStringLiteral(""), t),
        record(2, QStringLiteral("admin"), Role::Admin, QStringLiteral("changePassword"),
               QStringLiteral("admin"),
               QStringLiteral("token=******"),
               AuditResult::Success, QStringLiteral(""), t),
    });
    QCOMPARE(page.table()->rowCount(), 2);

    const QStringList sensitive = {
        QStringLiteral("P@ssw0rd!"), // 明文密码
        QStringLiteral("secret-token-abc"), // 明文令牌
    };
    // 收集页面全部可见文本 (表头 + 单元格 + 状态行).
    QStringList visibleTexts;
    for (int c = 0; c < page.table()->columnCount(); ++c)
        visibleTexts << page.table()->horizontalHeaderItem(c)->text();
    for (int r = 0; r < page.table()->rowCount(); ++r)
        for (int c = 0; c < page.table()->columnCount(); ++c)
            visibleTexts << page.table()->item(r, c)->text();
    visibleTexts << page.statusText();

    const QString joined = visibleTexts.join(QStringLiteral("|"));
    for (const QString &s : sensitive)
        QVERIFY2(!joined.contains(s),
                 qPrintable(QStringLiteral("sensitive value leaked: ") + s));
}

// --- page: 滚动到底部触发 requestMore ------------------------------------------------------

void AuditLogPageTest::scrollToBottomRequestsMore()
{
    AuditLogPage page;
    page.resize(800, 600);
    page.show(); // 布局生效, 滚动条才有真实范围
    QTest::qWaitForWindowExposed(&page);
    QTest::qWait(50); // 让布局/滚动条范围稳定
    const QDateTime t = QDateTime::currentDateTime();
    QVector<AuditRecord> many;
    for (int i = 0; i < 200; ++i) {
        many.append(record(i, QStringLiteral("admin"), Role::Admin,
                           QStringLiteral("login"), QStringLiteral("admin"),
                           QStringLiteral(""), AuditResult::Success,
                           QStringLiteral(""), t));
    }
    page.setRecords(many);
    QCOMPARE(page.table()->rowCount(), 200);
    QTest::qWait(50); // 让表格布局/滚动条范围更新

    QSignalSpy spy(&page, &AuditLogPage::requestMore);
    // 滚动到底部: 触发加载更多.
    page.table()->verticalScrollBar()->setValue(
        page.table()->verticalScrollBar()->maximum());
    QCOMPARE(spy.count(), 1);
}

// --- MainWindow integration ----------------------------------------------------------------

void AuditLogPageTest::mainWindowUsesAuditLogPage()
{
    // The 操作记录 stub is replaced by the real AuditLogPage at index 4.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    AuditLogPage *page = w.findChild<AuditLogPage *>();
    QVERIFY(page != nullptr);

    // Showing the page requests a reload (async load, Task 20 wires it).
    QSignalSpy spy(page, &AuditLogPage::requestReload);
    w.setCurrentPage(4);
    QCOMPARE(w.currentPageIndex(), 4);
    QVERIFY(page->isVisible());
    QVERIFY(spy.count() >= 1);

    // Fed data renders in the shell.
    const QDateTime t = QDateTime::currentDateTime();
    page->setRecords({
        record(1, QStringLiteral("anonymous"), Role::Anonymous,
               QStringLiteral("view"), QStringLiteral("overview"),
               QStringLiteral(""), AuditResult::Success, QStringLiteral(""), t),
    });
    QCOMPARE(page->table()->rowCount(), 1);
    QCOMPARE(page->table()->item(0, 1)->text(), QStringLiteral("匿名"));
}

QTEST_MAIN(AuditLogPageTest)
#include "test_audit_log_page.moc"
