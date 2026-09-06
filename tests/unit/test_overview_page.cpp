// Task 11 unit tests: 总览 page (spec §9, §11.3).
//
// Coverage required by the task brief:
// - Snapshot field mapping: each displayed field maps to the right snapshot
//   field (D120 step, D128/D130 widths, D210 delta, D122 speed, D138 production).
// - Stale/invalid fields display "—" (spec §9).
// - Read-only page: never sends write intents (no custom signals, no pending
//   commands, no MainWindow command signals).
// - Full-snapshot rendering: every refresh re-reads all fields from the model;
//   no per-field or optimistic updates.

#include <QtTest>
#include <QSignalSpy>
#include <QMouseEvent>
#include <QLabel>

#include "domain/device_snapshot.h"
#include "ui/shell/shell_model.h"
#include "ui/pages/overview_model.h"
#include "ui/pages/overview_page.h"
#include "ui/widgets/value_display.h"
#include "ui/MainWindow.h"

using namespace hlm;

namespace {

DeviceSnapshotData validSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.currentStep = 2;       // D120
    d.beltSpeed = 1500;      // D122
    d.targetWidth = 200;     // D128
    d.currentWidth = 150;    // D130
    d.widthDelta = -50;      // D210
    d.productionCount = 999; // D138
    d.heartbeat = 1;         // D140
    d.pulsePerMm = 1280;     // D204
    d.widthSpeed = 2;        // D220
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

class OverviewPageTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OverviewModel snapshot field mapping (spec §9) ----------------------
    void modelMapsSnapshotFields();
    void modelWithoutSnapshotShowsInvalid();
    void modelStaleSnapshotShowsInvalid();
    void modelOutOfRangeFieldShowsInvalid();
    void modelLatestAlarmText();

    // --- OverviewPage rendering ----------------------------------------------
    void pageRendersFullSnapshot();
    void pageStaleShowsDash();
    void pageShowsLatestAlarm();
    void pageFullSnapshotUpdateReplacesAllFields();

    // --- read-only page (no write intents) ------------------------------------
    void pageDeclaresNoSignals();
    void pageNeverSendsWriteIntents();

    // --- MainWindow integration ------------------------------------------------
    void mainWindowUsesOverviewPage();
    void mainInfoFitsOneScreen();
};

// --- OverviewModel snapshot field mapping -------------------------------------

void OverviewPageTest::modelMapsSnapshotFields()
{
    ShellModel model;
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    OverviewModel m(model);

    // Each field maps to the right snapshot register value.
    QCOMPARE(m.step().text, QStringLiteral("2"));            // D120
    QCOMPARE(m.targetWidth().text, QStringLiteral("200"));   // D128
    QCOMPARE(m.currentWidth().text, QStringLiteral("150"));  // D130
    QCOMPARE(m.widthDelta().text, QStringLiteral("-50"));    // D210 (signed)
    QCOMPARE(m.beltSpeed().text, QStringLiteral("1500"));    // D122
    QCOMPARE(m.productionCount().text, QStringLiteral("999"));// D138

    // Fresh snapshot with all fields in range: everything valid.
    QVERIFY(m.step().valid);
    QVERIFY(m.targetWidth().valid);
    QVERIFY(m.currentWidth().valid);
    QVERIFY(m.widthDelta().valid);
    QVERIFY(m.beltSpeed().valid);
    QVERIFY(m.productionCount().valid);

    // Status flags derive from the same snapshot.
    QVERIFY(m.online());
    QVERIFY(m.modeKnown());
}

void OverviewPageTest::modelWithoutSnapshotShowsInvalid()
{
    ShellModel model; // no snapshot yet
    OverviewModel m(model);

    // No confirmed snapshot: every field must be invalid -> "—" (spec §9).
    QVERIFY(!m.step().valid);
    QVERIFY(!m.targetWidth().valid);
    QVERIFY(!m.currentWidth().valid);
    QVERIFY(!m.widthDelta().valid);
    QVERIFY(!m.beltSpeed().valid);
    QVERIFY(!m.productionCount().valid);
    QVERIFY(!m.modeKnown());
    QVERIFY(!m.online());
}

void OverviewPageTest::modelStaleSnapshotShowsInvalid()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    d.dataAgeMs = 99999; // stale
    d.fastQuality = DataQuality::Stale;
    d.overallQuality = aggregateQuality(d);
    model.updateSnapshot(DeviceSnapshot(d));
    OverviewModel m(model);

    // Stale snapshot: fields must be invalid -> "—" (spec §9).
    QVERIFY(!m.step().valid);
    QVERIFY(!m.targetWidth().valid);
    QVERIFY(!m.currentWidth().valid);
    QVERIFY(!m.widthDelta().valid);
    QVERIFY(!m.beltSpeed().valid);
    QVERIFY(!m.productionCount().valid);
}

void OverviewPageTest::modelOutOfRangeFieldShowsInvalid()
{
    ShellModel model;
    DeviceSnapshotData d = validSnapshotData();
    d.targetWidth = 999; // D128 range is 50-400 -> out of range
    d.invalidFields = (quint32(1) << quint8(SnapshotField::TargetWidth));
    d.overallQuality = aggregateQuality(d);
    model.updateSnapshot(DeviceSnapshot(d));
    OverviewModel m(model);

    // The out-of-range field must be invalid -> "—" (spec §9). Freshness is
    // snapshot-wide (ShellModel::snapshotFresh), so the whole panel blanks
    // rather than showing a mix of trusted and untrusted values.
    QVERIFY(!m.targetWidth().valid);
    QVERIFY(!m.step().valid);
}

void OverviewPageTest::modelLatestAlarmText()
{
    ShellModel model;
    OverviewModel m(model);

    // No snapshot + offline: offline notice.
    QCOMPARE(m.latestAlarmText(), QStringLiteral("通讯中断"));

    // Fresh snapshot without fault: 无报警.
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QCOMPARE(m.latestAlarmText(), QStringLiteral("无报警"));

    // Fault code 3: the code's meaning is shown.
    DeviceSnapshotData d = validSnapshotData();
    d.faultCode = 3;
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(m.latestAlarmText().contains(QStringLiteral("安全光栅遮挡")));
}

// --- OverviewPage rendering ----------------------------------------------------

void OverviewPageTest::pageRendersFullSnapshot()
{
    ShellModel model;
    OverviewPage page(model);
    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));

    // ValueDisplay text is "value unit" when valid.
    QCOMPARE(page.fieldDisplay(QStringLiteral("step"))->text(),
             QStringLiteral("2"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("200 mm"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("currentWidth"))->text(),
             QStringLiteral("150 mm"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("widthDelta"))->text(),
             QStringLiteral("-50 mm"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("beltSpeed"))->text(),
             QStringLiteral("1500 Hz"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("productionCount"))->text(),
             QStringLiteral("999 件"));
    QVERIFY(page.fieldDisplay(QStringLiteral("step"))->isValid());
}

void OverviewPageTest::pageStaleShowsDash()
{
    ShellModel model;
    OverviewPage page(model);

    // No snapshot: every value shows "—" (spec §9, §11.2).
    for (const QString &key : {QStringLiteral("step"),
                               QStringLiteral("targetWidth"),
                               QStringLiteral("currentWidth"),
                               QStringLiteral("widthDelta"),
                               QStringLiteral("beltSpeed"),
                               QStringLiteral("productionCount")}) {
        QCOMPARE(page.fieldDisplay(key)->text(), QStringLiteral("—"));
        QVERIFY(!page.fieldDisplay(key)->isValid());
    }

    // A stale snapshot keeps everything at "—".
    DeviceSnapshotData d = validSnapshotData();
    d.fastQuality = DataQuality::Stale;
    d.overallQuality = aggregateQuality(d);
    model.updateSnapshot(DeviceSnapshot(d));
    QCOMPARE(page.fieldDisplay(QStringLiteral("step"))->text(),
             QStringLiteral("—"));
}

void OverviewPageTest::pageShowsLatestAlarm()
{
    ShellModel model;
    OverviewPage page(model);

    model.updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QCOMPARE(page.latestAlarmText(), QStringLiteral("无报警"));

    DeviceSnapshotData d = validSnapshotData();
    d.faultCode = 3;
    model.updateSnapshot(DeviceSnapshot(d));
    QVERIFY(page.latestAlarmText().contains(QStringLiteral("安全光栅遮挡")));
}

void OverviewPageTest::pageFullSnapshotUpdateReplacesAllFields()
{
    // Full snapshot updates only: a new snapshot replaces ALL displayed
    // fields; there are no per-field or optimistic updates (spec §9).
    ShellModel model;
    OverviewPage page(model);

    DeviceSnapshotData a = validSnapshotData();
    model.updateSnapshot(DeviceSnapshot(a));
    QCOMPARE(page.fieldDisplay(QStringLiteral("step"))->text(),
             QStringLiteral("2"));

    DeviceSnapshotData b = validSnapshotData();
    b.currentStep = 4;
    b.beltSpeed = 1200;
    b.targetWidth = 300;
    b.currentWidth = 300;
    b.widthDelta = 0;
    b.productionCount = 1000;
    model.updateSnapshot(DeviceSnapshot(b));

    QCOMPARE(page.fieldDisplay(QStringLiteral("step"))->text(),
             QStringLiteral("4"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("targetWidth"))->text(),
             QStringLiteral("300 mm"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("currentWidth"))->text(),
             QStringLiteral("300 mm"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("widthDelta"))->text(),
             QStringLiteral("0 mm"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("beltSpeed"))->text(),
             QStringLiteral("1200 Hz"));
    QCOMPARE(page.fieldDisplay(QStringLiteral("productionCount"))->text(),
             QStringLiteral("1000 件"));
}

// --- read-only page (no write intents) -----------------------------------------

void OverviewPageTest::pageDeclaresNoSignals()
{
    // The 总览 page is read-only (spec §11.3): it must not declare any signal
    // that could carry a write intent. Slots (refresh) are fine.
    const QMetaObject *mo = &OverviewPage::staticMetaObject;
    for (int i = QWidget::staticMetaObject.methodCount();
         i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        QVERIFY2(m.methodType() != QMetaMethod::Signal,
                 qPrintable(QStringLiteral("OverviewPage must not declare "
                                          "signals, found: %1")
                                .arg(m.methodSignature())));
    }
}

void OverviewPageTest::pageNeverSendsWriteIntents()
{
    // Interacting with the page must never produce a command: no MainWindow
    // command/mode signals, no pending commands in the shell model.
    MainWindow w;
    w.show();
    ShellModel *model = w.shellModel();
    QSignalSpy cmdSpy(&w, &MainWindow::commandRequested);
    QSignalSpy modeSpy(&w, &MainWindow::modeSwitchRequested);

    OverviewPage *page = w.findChild<OverviewPage *>();
    QVERIFY(page != nullptr);

    // Click every child widget of the page.
    const QList<QWidget *> children = page->findChildren<QWidget *>();
    for (QWidget *child : children)
        clickAt(child);
    QApplication::processEvents();

    // A snapshot update (the only input the page reacts to) also sends nothing.
    model->setUser(QStringLiteral("admin"), Role::Admin);
    model->setOnline(true);
    model->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    QCOMPARE(cmdSpy.count(), 0);
    QCOMPARE(modeSpy.count(), 0);
    QVERIFY(!model->hasPendingCommands());
    // The page still renders the snapshot inside the real shell.
    QCOMPARE(page->fieldDisplay(QStringLiteral("step"))->text(),
             QStringLiteral("2"));
}

// --- MainWindow integration ------------------------------------------------------

void OverviewPageTest::mainWindowUsesOverviewPage()
{
    // The 总览 stub is replaced by the real OverviewPage at index 0.
    MainWindow w;
    OverviewPage *page = w.findChild<OverviewPage *>();
    QVERIFY(page != nullptr);
    QCOMPARE(w.currentPageIndex(), 0);

    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QCOMPARE(page->fieldDisplay(QStringLiteral("step"))->text(),
             QStringLiteral("2"));
    QVERIFY(page->latestAlarmText().contains(QStringLiteral("无报警")));
}

void OverviewPageTest::mainInfoFitsOneScreen()
{
    // Acceptance: 主要信息一屏可读 — at the 1920x1080 baseline all main
    // fields are visible (Qt Layout only, no absolute coordinates).
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    OverviewPage *page = w.findChild<OverviewPage *>();
    QVERIFY(page != nullptr);
    w.shellModel()->updateSnapshot(DeviceSnapshot(validSnapshotData()));
    QApplication::processEvents();

    for (const QString &key : {QStringLiteral("step"),
                               QStringLiteral("targetWidth"),
                               QStringLiteral("currentWidth"),
                               QStringLiteral("widthDelta"),
                               QStringLiteral("beltSpeed"),
                               QStringLiteral("productionCount")}) {
        ValueDisplay *display = page->fieldDisplay(key);
        QVERIFY2(display->isVisible(),
                 qPrintable(QStringLiteral("display %1 not visible").arg(key)));
        QVERIFY(display->parentWidget() != nullptr);
        QCOMPARE(display->parentWidget()->objectName(),
                 QStringLiteral("valueField"));
    }
    QVERIFY(page->latestAlarmLabel()->isVisible());
}

QTEST_MAIN(OverviewPageTest)
#include "test_overview_page.moc"
