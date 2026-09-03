// Task 16 unit tests: I/O 与诊断 page (spec §8.2, §9, §11.3, §13).
//
// Coverage required by the task brief:
// - D100/D103 位展示: m0-m14、m30-m45 逐位正确.
// - M50-M53 / M100-M112 展示.
// - D140 活性: 心跳变化 = 活性, 不变 = 失效 (spec §13).
// - 过期值显示 "—" (spec §9).
// - 视觉失败隔离: setVisionStatus 失败后页面其他区域正常, 无控制命令
//   (spec §7.4, §13).
// - D102/D104/D105 只显示原始值 (hex), 不解析为位 (acceptance).
// - 只读页面: 不发送控制命令 (spec §11.3, §11.4).

#include <QtTest>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>

#include "domain/device_snapshot.h"
#include "ui/shell/shell_model.h"
#include "ui/pages/diagnostics_model.h"
#include "ui/pages/diagnostics_page.h"
#include "ui/widgets/value_display.h"
#include "ui/widgets/status_light.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.sequence = 1;
    d.statusWord1 = 0x0000; // D100
    d.statusWord2 = 0x0000; // D102
    d.statusWord3 = 0x0000; // D103
    d.statusWord4 = 0x0000; // D104
    d.statusWord5 = 0x0000; // D105
    d.faultCode = 0;        // D110
    d.currentStep = 2;      // D120
    d.beltSpeed = 1500;     // D122
    d.widthFrequency = 100; // D126/127
    d.targetWidth = 200;    // D128
    d.currentWidth = 150;   // D130
    d.pulseCount = 5000;    // D136/137
    d.productionCount = 999;// D138/139
    d.heartbeat = 1;        // D140
    d.pulsePerMm = 1280;    // D204
    d.widthDelta = -50;     // D210
    d.widthSpeed = 2;       // D220
    d.homeBits = 0;         // M50-M53
    d.commandBits = 0;      // M100-M112
    d.fastQuality = DataQuality::Valid;
    d.homeQuality = DataQuality::Valid;
    d.commandQuality = DataQuality::Valid;
    d.slowQuality = DataQuality::Valid;
    d.overallQuality = aggregateQuality(d);
    return d;
}

// Sends a synthetic click (press + release) at the widget center.
void clickAt(QWidget *w)
{
    const QPoint center = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, center,
                        w->mapToGlobal(center), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

} // namespace

class DiagnosticsPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- DiagnosticsModel: raw words (D100-D105) -----------------------------
    void modelMapsRawWords();
    void modelD102D104D105RawOnly();

    // --- DiagnosticsModel: D100 bits M0-M14 ----------------------------------
    void modelMapsD100Bits();

    // --- DiagnosticsModel: D103 bits M30-M45 ----------------------------------
    void modelMapsD103Bits();

    // --- DiagnosticsModel: M50-M53 / M100-M112 --------------------------------
    void modelMapsHomeAndCommandBits();

    // --- DiagnosticsModel: D140 heartbeat activity (spec §13) ----------------
    void modelHeartbeatActivity();
    void modelHeartbeatActivityIgnoresNonSnapshotChanges();

    // --- DiagnosticsModel: stale snapshot -> invalid (spec §9) ----------------
    void modelStaleShowsInvalid();

    // --- DiagnosticsModel: key registers --------------------------------------
    void modelMapsRegisters();

    // --- DiagnosticsModel: vision status + comm stats (Task 20 wiring) ---------
    void modelVisionStatus();
    void modelCommStats();

    // --- DiagnosticsPage rendering --------------------------------------------
    void pageRendersSnapshot();
    void pageStaleShowsDash();
    void pageShowsHeartbeatActivity();
    void pageHeartbeatShowsDashWhenStale();
    void pageShowsSectionTitles();
    void pageVisionFailureIsolated();
    void pageCommStatsRendered();

    // --- read-only page (no write intents) ------------------------------------
    void pageDeclaresNoSignals();
    void pageNeverSendsWriteIntents();

    // --- MainWindow integration ------------------------------------------------
    void mainWindowUsesDiagnosticsPage();
    void tableRowHeightAtLeast48();
    void mainDiagnosticsFitsOneScreen();
};

// --- raw words ----------------------------------------------------------------

void DiagnosticsPageTest::modelMapsRawWords()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = 0x0001; // D100
    d.statusWord2 = 0x1234; // D102
    d.statusWord3 = 0x00FF; // D103
    d.statusWord4 = 0xABCD; // D104
    d.statusWord5 = 0xFFFF; // D105
    model.updateSnapshot(DeviceSnapshot(d));
    DiagnosticsModel m(model);

    QVERIFY(m.rawWordsValid());
    QCOMPARE(m.rawWordHex(0), QStringLiteral("0x0001")); // D100
    QCOMPARE(m.rawWordHex(1), QStringLiteral("0x1234")); // D102
    QCOMPARE(m.rawWordHex(2), QStringLiteral("0x00FF")); // D103
    QCOMPARE(m.rawWordHex(3), QStringLiteral("0xABCD")); // D104
    QCOMPARE(m.rawWordHex(4), QStringLiteral("0xFFFF")); // D105
}

void DiagnosticsPageTest::modelD102D104D105RawOnly()
{
    // Acceptance: D102/D104/D105 只显示原始值, 不解析为位. The model exposes
    // them only as raw words; there is no bit accessor for them.
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    d.statusWord2 = 0x8000;
    d.statusWord4 = 0x0001;
    d.statusWord5 = 0x5555;
    model.updateSnapshot(DeviceSnapshot(d));
    DiagnosticsModel m(model);

    QCOMPARE(m.rawWordHex(1), QStringLiteral("0x8000"));
    QCOMPARE(m.rawWordHex(3), QStringLiteral("0x0001"));
    QCOMPARE(m.rawWordHex(4), QStringLiteral("0x5555"));
    // No bit accessor exists for D102/D104/D105 bits: bitState only accepts
    // the defined M numbers (M0-M14, M30-M45, M50-M53, M100-M112).
    QVERIFY(!m.bitState(200)); // M200 (D102) is not defined
    QVERIFY(!m.bitState(300)); // M300 (D104) is not defined
    QVERIFY(!m.bitState(316)); // M316 (D105) is not defined
}

// --- D100 bits M0-M14 ----------------------------------------------------------

void DiagnosticsPageTest::modelMapsD100Bits()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    // Bits 0,2,4,6,8,10,12,14 set (odd bits clear).
    d.statusWord1 = 0x5555;
    model.updateSnapshot(DeviceSnapshot(d));
    DiagnosticsModel m(model);

    for (int bit = 0; bit <= 14; ++bit) {
        const bool expected = (bit % 2 == 0);
        QCOMPARE(m.bitState(bit), expected);
    }
    // bit15 is reserved (spec §8.2): never exposed.
    QVERIFY(!m.bitState(15));
}

// --- D103 bits M30-M45 ---------------------------------------------------------

void DiagnosticsPageTest::modelMapsD103Bits()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    // D103 bit0-bit15 -> M30-M45. Set bits 0,4,12,13,14,15 (M30,M34,M42,M43,
    // M44,M45) and clear the rest.
    d.statusWord3 = 0xF011;
    model.updateSnapshot(DeviceSnapshot(d));
    DiagnosticsModel m(model);

    QVERIFY(m.bitState(30));  // bit0
    QVERIFY(!m.bitState(31)); // bit1
    QVERIFY(!m.bitState(32)); // bit2
    QVERIFY(!m.bitState(33)); // bit3
    QVERIFY(m.bitState(34));  // bit4
    QVERIFY(!m.bitState(35)); // bit5
    QVERIFY(!m.bitState(40)); // bit10
    QVERIFY(!m.bitState(41)); // bit11
    QVERIFY(m.bitState(42));  // bit12
    QVERIFY(m.bitState(43));  // bit13
    QVERIFY(m.bitState(44));  // bit14
    QVERIFY(m.bitState(45));  // bit15
}

// --- M50-M53 / M100-M112 -------------------------------------------------------

void DiagnosticsPageTest::modelMapsHomeAndCommandBits()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    d.homeBits = 0x000A;   // M51 + M53
    d.commandBits = 0x1001; // M100 + M112
    model.updateSnapshot(DeviceSnapshot(d));
    DiagnosticsModel m(model);

    QVERIFY(!m.bitState(50));
    QVERIFY(m.bitState(51));
    QVERIFY(!m.bitState(52));
    QVERIFY(m.bitState(53));

    QVERIFY(m.bitState(100));
    QVERIFY(!m.bitState(101));
    QVERIFY(!m.bitState(102));
    QVERIFY(!m.bitState(103));
    QVERIFY(!m.bitState(104));
    QVERIFY(!m.bitState(105));
    QVERIFY(!m.bitState(106));
    QVERIFY(!m.bitState(107));
    QVERIFY(!m.bitState(108));
    QVERIFY(!m.bitState(109));
    QVERIFY(!m.bitState(110));
    QVERIFY(!m.bitState(111));
    QVERIFY(m.bitState(112));
}

// --- D140 heartbeat activity (spec §13) ----------------------------------------

void DiagnosticsPageTest::modelHeartbeatActivity()
{
    ShellModel model;
    DiagnosticsModel m(model);

    // No snapshot yet: activity unknown.
    QVERIFY(!m.heartbeatKnown());

    // First snapshot: heartbeat observed, treated as active.
    DeviceSnapshotData a = validSnapshotData();
    a.sequence = 1;
    a.heartbeat = 1;
    model.updateSnapshot(DeviceSnapshot(a));
    QVERIFY(m.heartbeatKnown());
    QVERIFY(m.heartbeatActive());

    // Same heartbeat on the next snapshot: frozen -> inactive (spec §13).
    DeviceSnapshotData b = validSnapshotData();
    b.sequence = 2;
    b.heartbeat = 1;
    model.updateSnapshot(DeviceSnapshot(b));
    QVERIFY(!m.heartbeatActive());

    // Heartbeat changes again: active (16-bit wrap is fine, change-only).
    DeviceSnapshotData c = validSnapshotData();
    c.sequence = 3;
    c.heartbeat = 0; // wrapped from 1 -> 0
    model.updateSnapshot(DeviceSnapshot(c));
    QVERIFY(m.heartbeatActive());
}

void DiagnosticsPageTest::modelHeartbeatActivityIgnoresNonSnapshotChanges()
{
    // stateChanged also fires for user/online changes without a new snapshot;
    // activity must not flip because of those (it tracks snapshot sequence).
    ShellModel model;
    DiagnosticsModel m(model);

    DeviceSnapshotData a = validSnapshotData();
    a.sequence = 1;
    a.heartbeat = 1;
    model.updateSnapshot(DeviceSnapshot(a));
    QVERIFY(m.heartbeatActive());

    // Non-snapshot state change (user login): activity stays active.
    model.setUser(QStringLiteral("admin"), Role::Admin);
    QVERIFY(m.heartbeatActive());

    // A new snapshot with the same heartbeat: now frozen.
    DeviceSnapshotData b = validSnapshotData();
    b.sequence = 2;
    b.heartbeat = 1;
    model.updateSnapshot(DeviceSnapshot(b));
    QVERIFY(!m.heartbeatActive());

    // Another non-snapshot change: stays frozen.
    model.setUser(QStringLiteral("op"), Role::Operator);
    QVERIFY(!m.heartbeatActive());
}

// --- stale snapshot -> invalid (spec §9) ---------------------------------------

void DiagnosticsPageTest::modelStaleShowsInvalid()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    d.fastQuality = DataQuality::Stale;
    d.overallQuality = aggregateQuality(d);
    model.updateSnapshot(DeviceSnapshot(d));
    DiagnosticsModel m(model);

    QVERIFY(!m.rawWordsValid());
    QCOMPARE(m.rawWordHex(0), QStringLiteral("—"));
    QVERIFY(!m.bitValid());
    QVERIFY(!m.faultCode().valid);
    QVERIFY(!m.currentStep().valid);
    QVERIFY(!m.beltSpeed().valid);
    QVERIFY(!m.targetWidth().valid);
    QVERIFY(!m.currentWidth().valid);
    QVERIFY(!m.pulsePerMm().valid);
    QVERIFY(!m.widthSpeed().valid);
}

// --- key registers --------------------------------------------------------------

void DiagnosticsPageTest::modelMapsRegisters()
{
    ShellModel model;
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    DiagnosticsModel m(model);

    QCOMPARE(m.faultCode().text, QStringLiteral("0"));      // D110
    QCOMPARE(m.currentStep().text, QStringLiteral("2"));    // D120
    QCOMPARE(m.beltSpeed().text, QStringLiteral("1500"));   // D122
    QCOMPARE(m.widthFrequency().text, QStringLiteral("100"));// D126/127
    QCOMPARE(m.targetWidth().text, QStringLiteral("200"));  // D128
    QCOMPARE(m.currentWidth().text, QStringLiteral("150"));  // D130
    QCOMPARE(m.pulseCount().text, QStringLiteral("5000"));   // D136/137
    QCOMPARE(m.productionCount().text, QStringLiteral("999"));// D138/139
    QCOMPARE(m.pulsePerMm().text, QStringLiteral("1280"));   // D204
    QCOMPARE(m.widthDelta().text, QStringLiteral("-50"));    // D210
    QCOMPARE(m.widthSpeed().text, QStringLiteral("2"));      // D220

    QVERIFY(m.faultCode().valid);
    QVERIFY(m.currentStep().valid);
    QVERIFY(m.beltSpeed().valid);
    QVERIFY(m.widthFrequency().valid);
    QVERIFY(m.targetWidth().valid);
    QVERIFY(m.currentWidth().valid);
    QVERIFY(m.pulseCount().valid);
    QVERIFY(m.productionCount().valid);
    QVERIFY(m.pulsePerMm().valid);
    QVERIFY(m.widthDelta().valid);
    QVERIFY(m.widthSpeed().valid);
}

// --- vision status + comm stats -------------------------------------------------

void DiagnosticsPageTest::modelVisionStatus()
{
    ShellModel model;
    DiagnosticsModel m(model);

    // Default: unknown.
    QVERIFY(!m.visionHealthy());
    QVERIFY(m.visionVersion().isEmpty());
    QVERIFY(m.visionFailureReason().isEmpty());

    m.setVisionStatus(QStringLiteral("4.8.0"), true, QString());
    QCOMPARE(m.visionVersion(), QStringLiteral("4.8.0"));
    QVERIFY(m.visionHealthy());
    QVERIFY(m.visionFailureReason().isEmpty());

    m.setVisionStatus(QStringLiteral("4.8.0"), false,
                      QStringLiteral("OpenCV init failed"));
    QVERIFY(!m.visionHealthy());
    QCOMPARE(m.visionFailureReason(), QStringLiteral("OpenCV init failed"));
}

void DiagnosticsPageTest::modelCommStats()
{
    ShellModel model;
    DiagnosticsModel m(model);

    CommStats s;
    s.reconnectCount = 3;
    s.failedPolls = 7;
    m.setCommStats(s);
    QCOMPARE(m.commStats().reconnectCount, 3);
    QCOMPARE(m.commStats().failedPolls, 7);
}

// --- page rendering -------------------------------------------------------------

void DiagnosticsPageTest::pageRendersSnapshot()
{
    ShellModel model;
    DiagnosticsPage page(model);
    DeviceSnapshotData d = validSnapshotData();
    d.statusWord1 = 0x0001; // M0
    d.statusWord3 = 0x0010; // M34
    d.homeBits = 0x0002;    // M51
    d.commandBits = 0x0001; // M100
    model.updateSnapshot(DeviceSnapshot(d));

    // Raw word table: 5 rows (D100-D105), hex values.
    QCOMPARE(page.rawWordTable()->rowCount(), 5);
    QCOMPARE(page.rawWordTable()->item(0, 1)->text(), QStringLiteral("0x0001"));
    QCOMPARE(page.rawWordTable()->item(2, 1)->text(), QStringLiteral("0x0010"));

    // Bit tables: only the defined bits are shown.
    QCOMPARE(page.d100BitTable()->rowCount(), 15);   // M0-M14
    QCOMPARE(page.d103BitTable()->rowCount(), 12);   // M30-M35, M40-M45
    QCOMPARE(page.homeCommandTable()->rowCount(), 17); // M50-M53 + M100-M112

    // Spot-check bit states.
    QCOMPARE(page.d100BitTable()->item(0, 2)->text(), QStringLiteral("1"));   // M0
    QCOMPARE(page.d100BitTable()->item(1, 2)->text(), QStringLiteral("0"));   // M1
    QCOMPARE(page.d103BitTable()->item(4, 2)->text(), QStringLiteral("1"));   // M34
    QCOMPARE(page.homeCommandTable()->item(1, 2)->text(), QStringLiteral("1")); // M51
    QCOMPARE(page.homeCommandTable()->item(4, 2)->text(), QStringLiteral("1")); // M100

    // Registers.
    QCOMPARE(page.registerDisplay(QStringLiteral("faultCode"))->text(),
             QStringLiteral("0"));
    QCOMPARE(page.registerDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("200 mm"));
    QCOMPARE(page.registerDisplay(QStringLiteral("pulsePerMm"))->text(),
             QStringLiteral("1280 脉冲/mm"));
}

void DiagnosticsPageTest::pageStaleShowsDash()
{
    ShellModel model;
    DiagnosticsPage page(model);

    // No snapshot: everything shows "—".
    QCOMPARE(page.rawWordTable()->item(0, 1)->text(), QStringLiteral("—"));
    QCOMPARE(page.d100BitTable()->item(0, 2)->text(), QStringLiteral("—"));
    QCOMPARE(page.registerDisplay(QStringLiteral("faultCode"))->text(),
             QStringLiteral("—"));
    QCOMPARE(page.commDisplay(QStringLiteral("latency"))->text(),
             QStringLiteral("—"));

    // Stale snapshot keeps "—".
    DeviceSnapshotData d = validSnapshotData();
    d.fastQuality = DataQuality::Stale;
    d.overallQuality = aggregateQuality(d);
    model.updateSnapshot(DeviceSnapshot(d));
    QCOMPARE(page.rawWordTable()->item(0, 1)->text(), QStringLiteral("—"));
    QCOMPARE(page.d100BitTable()->item(0, 2)->text(), QStringLiteral("—"));
    QCOMPARE(page.registerDisplay(QStringLiteral("faultCode"))->text(),
             QStringLiteral("—"));
}

void DiagnosticsPageTest::pageShowsHeartbeatActivity()
{
    ShellModel model;
    DiagnosticsPage page(model);

    // No snapshot: heartbeat unknown -> "—".
    QCOMPARE(page.heartbeatLight()->text(), QStringLiteral("心跳 —"));

    DeviceSnapshotData a = validSnapshotData();
    a.sequence = 1;
    a.heartbeat = 1;
    model.updateSnapshot(DeviceSnapshot(a));
    QCOMPARE(page.heartbeatLight()->text(), QStringLiteral("心跳 活性"));

    DeviceSnapshotData b = validSnapshotData();
    b.sequence = 2;
    b.heartbeat = 1;
    model.updateSnapshot(DeviceSnapshot(b));
    QCOMPARE(page.heartbeatLight()->text(), QStringLiteral("心跳 失效"));
}

void DiagnosticsPageTest::pageHeartbeatShowsDashWhenStale()
{
    // Review finding: heartbeat light must not cache "活性" once the snapshot
    // goes stale/offline (spec §9: 过期值显示"—"; §13: D140 冻结 -> 数据失效).
    ShellModel model;
    DiagnosticsPage page(model);

    // Fresh snapshot: active.
    DeviceSnapshotData a = validSnapshotData();
    a.sequence = 1;
    a.heartbeat = 1;
    model.updateSnapshot(DeviceSnapshot(a));
    QCOMPARE(page.heartbeatLight()->text(), QStringLiteral("心跳 活性"));

    // Snapshot goes stale (PLC disconnected): light must show "—", not the
    // cached "活性".
    DeviceSnapshotData stale = validSnapshotData();
    stale.sequence = 2;
    stale.heartbeat = 1;
    stale.fastQuality = DataQuality::Stale;
    stale.overallQuality = aggregateQuality(stale);
    model.updateSnapshot(DeviceSnapshot(stale));
    QCOMPARE(page.heartbeatLight()->text(), QStringLiteral("心跳 —"));

    // First snapshot already stale: never shows "活性".
    ShellModel model2;
    DiagnosticsPage page2(model2);
    model2.updateSnapshot(DeviceSnapshot(stale));
    QCOMPARE(page2.heartbeatLight()->text(), QStringLiteral("心跳 —"));
}

void DiagnosticsPageTest::pageShowsSectionTitles()
{
    // Review finding: the register/comm section titles were lost because the
    // ValueDisplay was reparented out of its wrapper. The wrapper (title label
    // + value) must be added to the layout and stay visible.
    ShellModel model;
    DiagnosticsPage page(model);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    page.show();
    QApplication::processEvents();
    const QList<QLabel *> labels = page.findChildren<QLabel *>();
    auto findTitle = [&labels](const QString &text) -> QLabel * {
        for (QLabel *l : labels)
            if (l->text() == text)
                return l;
        return nullptr;
    };

    QLabel *belt = findTitle(QStringLiteral("皮带速度 (D122)"));
    QVERIFY(belt != nullptr);
    QVERIFY(belt->isVisible());
    QCOMPARE(belt->text(), QStringLiteral("皮带速度 (D122)"));

    QLabel *fault = findTitle(QStringLiteral("故障代码 (D110)"));
    QVERIFY(fault != nullptr);
    QVERIFY(fault->isVisible());

    QLabel *latency = findTitle(QStringLiteral("最近快照延迟"));
    QVERIFY(latency != nullptr);
    QVERIFY(latency->isVisible());

    QLabel *reconnect = findTitle(QStringLiteral("重连次数"));
    QVERIFY(reconnect != nullptr);
    QVERIFY(reconnect->isVisible());
}

void DiagnosticsPageTest::pageVisionFailureIsolated()
{
    // spec §7.4/§13: vision self-test failure only marks the vision diagnostic
    // red; PLC data display and the rest of the page stay normal.
    ShellModel model;
    DiagnosticsPage page(model);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    page.setVisionStatus(QStringLiteral("4.8.0"), false,
                         QStringLiteral("OpenCV init failed"));

    // Vision section shows the failure.
    QCOMPARE(page.visionVersionLabel()->text(), QStringLiteral("4.8.0"));
    QVERIFY(page.visionStatusLabel()->text().contains(QStringLiteral("失败")));
    QCOMPARE(page.visionFailureLabel()->text(),
             QStringLiteral("OpenCV init failed"));

    // The rest of the page is unaffected: raw words, bits and registers still
    // render from the snapshot.
    QCOMPARE(page.rawWordTable()->item(0, 1)->text(), QStringLiteral("0x0000"));
    QCOMPARE(page.d100BitTable()->item(0, 2)->text(), QStringLiteral("0"));
    QCOMPARE(page.registerDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("200 mm"));
    QCOMPARE(page.heartbeatLight()->text(), QStringLiteral("心跳 活性"));
}

void DiagnosticsPageTest::pageCommStatsRendered()
{
    ShellModel model;
    DiagnosticsPage page(model);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    CommStats s;
    s.reconnectCount = 3;
    s.failedPolls = 7;
    page.setCommStats(s);

    QCOMPARE(page.commDisplay(QStringLiteral("reconnect"))->text(),
             QStringLiteral("3"));
    QCOMPARE(page.commDisplay(QStringLiteral("failedPolls"))->text(),
             QStringLiteral("7"));
    QCOMPARE(page.commDisplay(QStringLiteral("latency"))->text(),
             QStringLiteral("0 ms"));
    QCOMPARE(page.commDisplay(QStringLiteral("sequence"))->text(),
             QStringLiteral("1"));
}

// --- read-only page (no write intents) -----------------------------------------

void DiagnosticsPageTest::pageDeclaresNoSignals()
{
    // The I/O 与诊断 page is read-only (spec §11.3): it must not declare any
    // signal that could carry a write intent. Slots (refresh, setVisionStatus,
    // setCommStats) are fine.
    const QMetaObject *mo = &DiagnosticsPage::staticMetaObject;
    for (int i = QWidget::staticMetaObject.methodCount();
         i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        QVERIFY2(m.methodType() != QMetaMethod::Signal,
                 qPrintable(QStringLiteral("DiagnosticsPage must not declare "
                                           "signals, found: %1")
                                .arg(m.methodSignature())));
    }
}

void DiagnosticsPageTest::pageNeverSendsWriteIntents()
{
    // Interacting with the page must never produce a command: no MainWindow
    // command/mode signals, no pending commands in the shell model.
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    QSignalSpy cmdSpy(&w, &MainWindow::commandRequested);
    QSignalSpy modeSpy(&w, &MainWindow::modeSwitchRequested);

    DiagnosticsPage *page = w.findChild<DiagnosticsPage *>();
    QVERIFY(page != nullptr);

    // Click every child widget of the page.
    const QList<QWidget *> children = page->findChildren<QWidget *>();
    for (QWidget *child : children)
        clickAt(child);
    QApplication::processEvents();

    // A snapshot update and a vision failure also send nothing.
    model->setUser(QStringLiteral("admin"), Role::Admin);
    model->setOnline(true);
    model->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    page->setVisionStatus(QStringLiteral("4.8.0"), false, QStringLiteral("x"));
    QApplication::processEvents();

    QCOMPARE(cmdSpy.count(), 0);
    QCOMPARE(modeSpy.count(), 0);
    QVERIFY(!model->hasPendingCommands());
    // The page still renders the snapshot inside the real shell.
    QCOMPARE(page->registerDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("200 mm"));
}

// --- MainWindow integration ------------------------------------------------------

void DiagnosticsPageTest::mainWindowUsesDiagnosticsPage()
{
    // The I/O 与诊断 stub is replaced by the real DiagnosticsPage at index 5.
    MainWindow w;
    DiagnosticsPage *page = w.findChild<DiagnosticsPage *>();
    QVERIFY(page != nullptr);
    QCOMPARE(w.currentPageIndex(), 0);

    w.setCurrentPage(5);
    QCOMPARE(w.currentPageIndex(), 5);

    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QCOMPARE(page->registerDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("200 mm"));
}

void DiagnosticsPageTest::tableRowHeightAtLeast48()
{
    // 表格行高不得小于 48 px (spec §11.1).
    ShellModel model;
    DiagnosticsPage page(model);
    for (QTableWidget *table : {page.rawWordTable(), page.d100BitTable(),
                                page.d103BitTable(), page.homeCommandTable()}) {
        QVERIFY(table->verticalHeader()->defaultSectionSize() >= 48);
        QVERIFY(table->verticalHeader()->minimumSectionSize() >= 48);
    }
}

void DiagnosticsPageTest::mainDiagnosticsFitsOneScreen()
{
    // At the 1920x1080 baseline all main sections are visible (Qt Layout only,
    // no absolute coordinates; the page scrolls if the window is smaller).
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    DiagnosticsPage *page = w.findChild<DiagnosticsPage *>();
    QVERIFY(page != nullptr);
    w.setCurrentPage(5);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    QVERIFY(page->rawWordTable()->isVisible());
    QVERIFY(page->d100BitTable()->isVisible());
    QVERIFY(page->d103BitTable()->isVisible());
    QVERIFY(page->homeCommandTable()->isVisible());
    QVERIFY(page->registerDisplay(QStringLiteral("targetWidth"))->isVisible());
    QVERIFY(page->visionVersionLabel()->isVisible());
}

QTEST_MAIN(DiagnosticsPageTest)
#include "test_diagnostics_page.moc"
