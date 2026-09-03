// Task 14 unit tests: 报警 page (spec §11.3, §12).
//
// Coverage required by the task brief:
// - 活动/恢复区分: endedAt invalid = 活动 (current), valid = 已恢复 (history).
// - 未知非零 PLC 代码显示通用文字, 绝不崩溃 (spec §12).
// - 日期筛选 (startedAt 范围) 和代码筛选 (PLC 代码 / HMI 按 source 或
//   messageSnapshot 匹配).
// - 异步加载: setLoading / setAlarms / setLoadFailed 状态与无数据状态.
// - 表格行高 >= 48 px (spec §11.1).
// - 无确认语义: 页面无确认控件, 无确认状态 (spec §12).
// - 只读页面: 除 requestReload 外无任何信号, 不发控制命令.

#include <QtTest>
#include <QSignalSpy>
#include <QLabel>
#include <QLineEdit>
#include <QDateEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>

#include "domain/fault_code.h"
#include "ui/pages/alarm_model.h"
#include "ui/pages/alarm_page.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

AlarmEventRecord alarm(qint64 id, AlarmSource source, quint16 code,
                       const QString &message, AlarmSeverity severity,
                       const QDateTime &startedAt, const QDateTime &endedAt)
{
    AlarmEventRecord r;
    r.id = id;
    r.source = source;
    r.code = code;
    r.messageSnapshot = message;
    r.severity = severity;
    r.startedAt = startedAt;
    r.endedAt = endedAt;
    return r;
}

} // namespace

class AlarmPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- model: 活动/恢复区分 (spec §12) --------------------------------------
    void activeVsRecovered();

    // --- model: 未知代码通用文字, 绝不崩溃 (spec §12) --------------------------
    void unknownCodeShowsGenericTextWithoutCrash();
    void knownCodeShowsMeaning();
    void hmiRecordShowsMessageSnapshot();

    // --- model: 日期筛选 --------------------------------------------------------
    void dateFilterRangesOnStartedAt();

    // --- model: 代码筛选 (PLC 代码 / HMI 按 source 或 messageSnapshot) ----------
    void codeFilterMatchesPlcCodeAndHmiMessage();

    // --- model: 异步加载与无数据状态 --------------------------------------------
    void asyncLoadStates();
    void emptyLoadShowsNoDataState();

    // --- page: 48px 行高与触摸目标 (spec §11.1) ---------------------------------
    void tableRowHeightAtLeast48();
    void filterControlsMeetTouchTargetSize();

    // --- page: 无确认语义 + 只读 (spec §12, §11.3) ------------------------------
    void noConfirmControls();
    void onlySignalIsRequestReload();

    // --- page: 渲染 --------------------------------------------------------------
    void pageRendersFilteredRows();

    // --- page: 日期筛选控件与模型联动 (review finding 回归) ----------------------
    void dateEditsDefaultToAllAndMatchModel();
    void dateEditChangeFiltersModel();
    void dateEditResetToAllClearsFilter();
    void invalidDateRangeShowsMessage();

    // --- page: showEvent 触发 reload, 窗口内去重 (review finding) ------------------
    void showEventReloadsOnFirstShow();
    void showEventReloadsOnPageReentry();
    void rapidDoubleShowDoesNotDoubleReload();

    // --- MainWindow integration --------------------------------------------------
    void mainWindowUsesAlarmPage();
};

// --- model: 活动/恢复区分 ---------------------------------------------------------

void AlarmPageTest::activeVsRecovered()
{
    const QDateTime t = QDateTime::currentDateTime();
    AlarmEventRecord active = alarm(1, AlarmSource::Plc, 1,
                                    QStringLiteral("急停"), AlarmSeverity::Critical,
                                    t, QDateTime()); // endedAt invalid
    AlarmEventRecord recovered = alarm(2, AlarmSource::Plc, 2,
                                       QStringLiteral("安全门打开"),
                                       AlarmSeverity::Warning,
                                       t, t.addSecs(60));

    QVERIFY(active.isActive());
    QVERIFY(!recovered.isActive());
    QCOMPARE(AlarmModel::statusText(active), QStringLiteral("活动"));
    QCOMPARE(AlarmModel::statusText(recovered), QStringLiteral("已恢复"));
}

// --- model: 未知代码通用文字 ------------------------------------------------------

void AlarmPageTest::unknownCodeShowsGenericTextWithoutCrash()
{
    // Unknown non-zero PLC code: generic text, code preserved, never crash
    // (spec §12: 未知非零代码照常保存原始值和通用文字).
    const QDateTime t = QDateTime::currentDateTime();
    AlarmEventRecord r = alarm(1, AlarmSource::Plc, 99,
                               QStringLiteral(""), AlarmSeverity::Critical,
                               t, QDateTime());

    QCOMPARE(AlarmModel::codeText(r), QStringLiteral("99"));
    QVERIFY(AlarmModel::meaningText(r).contains(QStringLiteral("未知锁存故障")));
    // FaultCodeTable itself must not crash either.
    QCOMPARE(FaultCodeTable::instance().info(99).code, quint16(99));
}

void AlarmPageTest::knownCodeShowsMeaning()
{
    const QDateTime t = QDateTime::currentDateTime();
    AlarmEventRecord r = alarm(1, AlarmSource::Plc, 1,
                               QStringLiteral(""), AlarmSeverity::Critical,
                               t, QDateTime());
    QCOMPARE(AlarmModel::codeText(r), QStringLiteral("1"));
    QCOMPARE(AlarmModel::meaningText(r), QStringLiteral("急停"));
}

void AlarmPageTest::hmiRecordShowsMessageSnapshot()
{
    // HMI alarms have no PLC code: code column "—", meaning from the
    // messageSnapshot (spec §12: HMI 系统报警至少包含通讯中断等).
    const QDateTime t = QDateTime::currentDateTime();
    AlarmEventRecord r = alarm(1, AlarmSource::Hmi, 0,
                               QStringLiteral("通讯中断"), AlarmSeverity::Warning,
                               t, QDateTime());
    QCOMPARE(AlarmModel::codeText(r), QStringLiteral("—"));
    QCOMPARE(AlarmModel::meaningText(r), QStringLiteral("通讯中断"));
    QCOMPARE(AlarmModel::sourceText(r.source), QStringLiteral("HMI"));
}

// --- model: 日期筛选 --------------------------------------------------------------

void AlarmPageTest::dateFilterRangesOnStartedAt()
{
    AlarmModel m;
    const QDateTime d1(QDate(2026, 8, 1), QTime(10, 0));
    const QDateTime d2(QDate(2026, 8, 15), QTime(10, 0));
    const QDateTime d3(QDate(2026, 9, 1), QTime(10, 0));
    m.setAlarms({
        alarm(1, AlarmSource::Plc, 1, QStringLiteral("急停"),
              AlarmSeverity::Critical, d1, QDateTime()),
        alarm(2, AlarmSource::Plc, 2, QStringLiteral("安全门打开"),
              AlarmSeverity::Warning, d2, QDateTime()),
        alarm(3, AlarmSource::Plc, 3, QStringLiteral("安全光栅遮挡"),
              AlarmSeverity::Warning, d3, QDateTime()),
    });
    QCOMPARE(m.rowCount(), 3);

    // From-only: 2026-08-15 onwards.
    m.setDateRange(QDate(2026, 8, 15), std::nullopt);
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(m.alarmAt(0).id, qint64(2));
    QCOMPARE(m.alarmAt(1).id, qint64(3));

    // To-only: up to 2026-08-15.
    m.setDateRange(std::nullopt, QDate(2026, 8, 15));
    QCOMPARE(m.rowCount(), 2);
    QCOMPARE(m.alarmAt(0).id, qint64(1));
    QCOMPARE(m.alarmAt(1).id, qint64(2));

    // Both: exactly 2026-08-15.
    m.setDateRange(QDate(2026, 8, 15), QDate(2026, 8, 15));
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.alarmAt(0).id, qint64(2));

    // Cleared: all rows again.
    m.setDateRange(std::nullopt, std::nullopt);
    QCOMPARE(m.rowCount(), 3);
}

// --- model: 代码筛选 --------------------------------------------------------------

void AlarmPageTest::codeFilterMatchesPlcCodeAndHmiMessage()
{
    AlarmModel m;
    const QDateTime t = QDateTime::currentDateTime();
    m.setAlarms({
        alarm(1, AlarmSource::Plc, 3, QStringLiteral("安全光栅遮挡"),
              AlarmSeverity::Warning, t, QDateTime()),
        alarm(2, AlarmSource::Plc, 1, QStringLiteral("急停"),
              AlarmSeverity::Critical, t, QDateTime()),
        alarm(3, AlarmSource::Hmi, 0, QStringLiteral("通讯中断"),
              AlarmSeverity::Warning, t, QDateTime()),
    });

    // PLC: filter matches the code number.
    m.setCodeFilter(QStringLiteral("3"));
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.alarmAt(0).id, qint64(1));

    // HMI (no code): filter matches the messageSnapshot.
    m.setCodeFilter(QStringLiteral("通讯中断"));
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.alarmAt(0).id, qint64(3));

    // HMI: filter matches the source name too.
    m.setCodeFilter(QStringLiteral("HMI"));
    QCOMPARE(m.rowCount(), 1);
    QCOMPARE(m.alarmAt(0).id, qint64(3));

    // Empty filter: everything.
    m.setCodeFilter(QString());
    QCOMPARE(m.rowCount(), 3);
}

// --- model: 异步加载与无数据状态 --------------------------------------------------

void AlarmPageTest::asyncLoadStates()
{
    AlarmModel m;
    QCOMPARE(m.loadState(), AlarmModel::LoadState::Idle);
    QVERIFY(m.statusText().contains(QStringLiteral("尚未加载")));

    m.setLoading();
    QCOMPARE(m.loadState(), AlarmModel::LoadState::Loading);
    QVERIFY(m.statusText().contains(QStringLiteral("加载中")));

    const QDateTime t = QDateTime::currentDateTime();
    m.setAlarms({alarm(1, AlarmSource::Plc, 1, QStringLiteral("急停"),
                       AlarmSeverity::Critical, t, QDateTime())});
    QCOMPARE(m.loadState(), AlarmModel::LoadState::Loaded);
    QCOMPARE(m.rowCount(), 1);
    QVERIFY(m.statusText().contains(QStringLiteral("共 1 条")));

    // Failure is explicit and clears on the next successful load.
    m.setLoadFailed(QStringLiteral("数据库不可用"));
    QCOMPARE(m.loadState(), AlarmModel::LoadState::Failed);
    QVERIFY(m.statusText().contains(QStringLiteral("加载失败")));
    QVERIFY(m.statusText().contains(QStringLiteral("数据库不可用")));

    m.setAlarms({});
    QCOMPARE(m.loadState(), AlarmModel::LoadState::Loaded);
    QVERIFY(!m.statusText().contains(QStringLiteral("加载失败")));
}

void AlarmPageTest::emptyLoadShowsNoDataState()
{
    AlarmModel m;
    m.setAlarms({});
    QCOMPARE(m.loadState(), AlarmModel::LoadState::Loaded);
    QCOMPARE(m.rowCount(), 0);
    QVERIFY(m.isEmpty());
    QVERIFY(m.statusText().contains(QStringLiteral("无报警记录")));
}

// --- page: 48px 行高与触摸目标 -----------------------------------------------------

void AlarmPageTest::tableRowHeightAtLeast48()
{
    // 表格行高不得小于 48 px (spec §11.1).
    AlarmPage page;
    QVERIFY(page.table()->verticalHeader()->defaultSectionSize() >= 48);
    QVERIFY(page.table()->verticalHeader()->minimumSectionSize() >= 48);
}

void AlarmPageTest::filterControlsMeetTouchTargetSize()
{
    // 触摸目标 >= 48 px (spec §11.1).
    AlarmPage page;
    QVERIFY(page.dateFromEdit()->minimumHeight() >= 48);
    QVERIFY(page.dateToEdit()->minimumHeight() >= 48);
    QVERIFY(page.codeFilterEdit()->minimumHeight() >= 48);
    QVERIFY(page.reloadButton()->minimumHeight() >= 48);
}

// --- page: 无确认语义 + 只读 ------------------------------------------------------

void AlarmPageTest::noConfirmControls()
{
    // spec §12: 不提供没有 PLC 地址支撑的"报警确认"语义. The page must not
    // contain any confirm/acknowledge control or state.
    AlarmPage page;
    const auto buttons = page.findChildren<QPushButton *>();
    for (const QPushButton *b : buttons)
        QVERIFY2(!b->text().contains(QStringLiteral("确认")),
                 qPrintable(QStringLiteral("unexpected confirm control: ") + b->text()));
    // The only button is the reload button.
    QCOMPARE(buttons.size(), 1);
    QCOMPARE(buttons.first(), page.reloadButton());
}

void AlarmPageTest::onlySignalIsRequestReload()
{
    // 页面只读, 不发任何控制命令 (spec §11.3): the only signal is the data
    // request. Any control-command signal would fail this test.
    AlarmPage page;
    QStringList signalNames;
    const QMetaObject *mo = page.metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        const QMetaMethod method = mo->method(i);
        // Only signals declared by AlarmPage itself (inherited QObject signals
        // like destroyed() are not page intents).
        if (method.methodType() == QMetaMethod::Signal
            && method.enclosingMetaObject() == mo)
            signalNames << QString::fromLatin1(method.name());
    }
    QCOMPARE(signalNames, QStringList{QStringLiteral("requestReload")});
}

// --- page: 渲染 --------------------------------------------------------------------

void AlarmPageTest::pageRendersFilteredRows()
{
    AlarmPage page;
    const QDateTime t = QDateTime::currentDateTime();
    page.setAlarms({
        alarm(1, AlarmSource::Plc, 1, QStringLiteral("急停"),
              AlarmSeverity::Critical, t, QDateTime()),
        alarm(2, AlarmSource::Plc, 2, QStringLiteral("安全门打开"),
              AlarmSeverity::Warning, t, t.addSecs(60)),
    });
    QCOMPARE(page.table()->rowCount(), 2);

    // Status column (0): 活动 / 已恢复.
    QCOMPARE(page.table()->item(0, 0)->text(), QStringLiteral("活动"));
    QCOMPARE(page.table()->item(1, 0)->text(), QStringLiteral("已恢复"));
    // Code column (2) and meaning column (3).
    QCOMPARE(page.table()->item(0, 2)->text(), QStringLiteral("1"));
    QCOMPARE(page.table()->item(0, 3)->text(), QStringLiteral("急停"));
    QCOMPARE(page.table()->item(1, 2)->text(), QStringLiteral("2"));
    QCOMPARE(page.table()->item(1, 3)->text(), QStringLiteral("安全门打开"));

    // Filtering re-renders the table (real user path: the filter widget).
    page.codeFilterEdit()->setText(QStringLiteral("2"));
    QCOMPARE(page.table()->rowCount(), 1);
    QCOMPARE(page.table()->item(0, 2)->text(), QStringLiteral("2"));
}

// --- page: 日期筛选控件与模型联动 (review finding 回归) --------------------------

void AlarmPageTest::dateEditsDefaultToAllAndMatchModel()
{
    // 初始状态: 两个日期字段都显示"全部" (哨兵日期), 模型无日期筛选 —
    // 显示值与筛选状态一致 (review finding: 初始日期从未推给模型).
    AlarmPage page;
    QCOMPARE(page.dateFromEdit()->text(), QStringLiteral("全部"));
    QCOMPARE(page.dateToEdit()->text(), QStringLiteral("全部"));
    QVERIFY(!page.model()->dateFrom().has_value());
    QVERIFY(!page.model()->dateTo().has_value());
}

void AlarmPageTest::dateEditChangeFiltersModel()
{
    AlarmPage page;
    const QDateTime d1(QDate(2026, 8, 1), QTime(10, 0));
    const QDateTime d2(QDate(2026, 8, 15), QTime(10, 0));
    const QDateTime d3(QDate(2026, 9, 1), QTime(10, 0));
    page.setAlarms({
        alarm(1, AlarmSource::Plc, 1, QStringLiteral("急停"),
              AlarmSeverity::Critical, d1, QDateTime()),
        alarm(2, AlarmSource::Plc, 2, QStringLiteral("安全门打开"),
              AlarmSeverity::Warning, d2, QDateTime()),
        alarm(3, AlarmSource::Plc, 3, QStringLiteral("安全光栅遮挡"),
              AlarmSeverity::Warning, d3, QDateTime()),
    });
    QCOMPARE(page.table()->rowCount(), 3);

    // 设置"从"日期: 字段显示真实日期, 模型同步筛选.
    page.dateFromEdit()->setDate(QDate(2026, 8, 15));
    QCOMPARE(page.model()->dateFrom(), std::optional<QDate>(QDate(2026, 8, 15)));
    QCOMPARE(page.table()->rowCount(), 2);
    QCOMPARE(page.table()->item(0, 2)->text(), QStringLiteral("2"));

    // 设置"至"日期: 字段与模型一致.
    page.dateToEdit()->setDate(QDate(2026, 8, 15));
    QCOMPARE(page.model()->dateTo(), std::optional<QDate>(QDate(2026, 8, 15)));
    QCOMPARE(page.table()->rowCount(), 1);
    QCOMPARE(page.table()->item(0, 2)->text(), QStringLiteral("2"));
}

void AlarmPageTest::dateEditResetToAllClearsFilter()
{
    AlarmPage page;
    const QDateTime t = QDateTime::currentDateTime();
    page.setAlarms({
        alarm(1, AlarmSource::Plc, 1, QStringLiteral("急停"),
              AlarmSeverity::Critical, t, QDateTime()),
        alarm(2, AlarmSource::Plc, 2, QStringLiteral("安全门打开"),
              AlarmSeverity::Warning, t, QDateTime()),
    });
    page.dateFromEdit()->setDate(QDate(2026, 8, 15));
    page.dateToEdit()->setDate(QDate(2026, 8, 20));
    QCOMPARE(page.table()->rowCount(), 0);

    // 重置回"全部": 模型筛选清除, 显示值与模型一致.
    page.dateFromEdit()->setDate(page.dateFromEdit()->minimumDate());
    QCOMPARE(page.dateFromEdit()->text(), QStringLiteral("全部"));
    QVERIFY(!page.model()->dateFrom().has_value());
    QCOMPARE(page.table()->rowCount(), 0); // "至" 仍生效

    page.dateToEdit()->setDate(page.dateToEdit()->minimumDate());
    QCOMPARE(page.dateToEdit()->text(), QStringLiteral("全部"));
    QVERIFY(!page.model()->dateTo().has_value());
    QCOMPARE(page.table()->rowCount(), 2);
}

void AlarmPageTest::invalidDateRangeShowsMessage()
{
    AlarmPage page;
    const QDateTime t = QDateTime::currentDateTime();
    page.setAlarms({
        alarm(1, AlarmSource::Plc, 1, QStringLiteral("急停"),
              AlarmSeverity::Critical, t, QDateTime()),
    });
    // from > to: 无有效范围, 不匹配任何记录, 状态行给出明确提示.
    page.dateFromEdit()->setDate(QDate(2026, 9, 1));
    page.dateToEdit()->setDate(QDate(2026, 8, 1));
    QCOMPARE(page.table()->rowCount(), 0);
    QVERIFY(page.statusText().contains(QStringLiteral("起止日期无效")));

    // 修正范围后恢复.
    page.dateToEdit()->setDate(QDate(2026, 9, 30));
    QCOMPARE(page.table()->rowCount(), 1);
    QVERIFY(page.statusText().contains(QStringLiteral("共 1 条")));
}

// --- page: showEvent 触发 reload, 窗口内去重 (review finding) --------------------------

void AlarmPageTest::showEventReloadsOnFirstShow()
{
    AlarmPage page;
    QSignalSpy spy(&page, &AlarmPage::requestReload);
    page.show();
    QCOMPARE(spy.count(), 1);
}

void AlarmPageTest::showEventReloadsOnPageReentry()
{
    // 页面切换: 离开 报警 再回来 (show → hide → show) 应再次请求新数据,
    // 否则报警会一直陈旧直到手动刷新 (review finding).
    AlarmPage page;
    QSignalSpy spy(&page, &AlarmPage::requestReload);
    page.show();
    QCOMPARE(spy.count(), 1);

    page.hide();
    // 真实页面切换间隔远超去重窗口 (500ms); 等待窗口过后再回来.
    QTest::qWait(600);
    page.show();
    QCOMPARE(spy.count(), 2);
}

void AlarmPageTest::rapidDoubleShowDoesNotDoubleReload()
{
    // 窗口从最小化恢复时会在短时间内连续触发多个 showEvent: 窗口内去重,
    // 不产生冗余 reload (review finding).
    AlarmPage page;
    QSignalSpy spy(&page, &AlarmPage::requestReload);
    page.show();
    QCOMPARE(spy.count(), 1);

    page.hide();
    page.show();
    page.hide();
    page.show();
    QCOMPARE(spy.count(), 1);
}

// --- MainWindow integration ----------------------------------------------------------

void AlarmPageTest::mainWindowUsesAlarmPage()
{
    // The 报警 stub is replaced by the real AlarmPage at index 3.
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    AlarmPage *page = w.findChild<AlarmPage *>();
    QVERIFY(page != nullptr);

    // Showing the page requests a reload (async load, Task 20 wires it).
    QSignalSpy spy(page, &AlarmPage::requestReload);
    w.setCurrentPage(3);
    QCOMPARE(w.currentPageIndex(), 3);
    QVERIFY(page->isVisible());
    QVERIFY(spy.count() >= 1);

    // Fed data renders in the shell.
    const QDateTime t = QDateTime::currentDateTime();
    page->setAlarms({alarm(1, AlarmSource::Plc, 1, QStringLiteral("急停"),
                           AlarmSeverity::Critical, t, QDateTime())});
    QCOMPARE(page->table()->rowCount(), 1);
    QCOMPARE(page->table()->item(0, 3)->text(), QStringLiteral("急停"));
}

QTEST_MAIN(AlarmPageTest)
#include "test_alarm_page.moc"
