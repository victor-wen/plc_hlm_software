// Developer integration test: barcode ingestion end to end (user decision
// 2026-09-22, revised the same day — the HMI drives the scan through the vendor
// SDK instead of reading a result file).
//
// The HMI's whole job in one line: watch the PLC's M15 扫码结束 coil, and on
// its rising edge run one decode cycle and show what came back.
//
// This test drives the real composition root (Application) with the in-process
// PLC simulator and an injected fake SDK, so the wiring under test is the
// production one: settings load/save through the SQLite app_settings table, the
// M15 rising edge on the snapshot feed, the worker-thread trigger/poll cycle,
// the append to the configured file, and the overview page rendering. The
// vendor DLL is the only thing swapped out.
//
// Guarantees pinned here:
//   - with no configured path the surface stays 未配置 and nothing is triggered;
//   - a confirmed path save is echoed and reaches the adapter;
//   - M15 rising runs exactly one cycle (edge-triggered, fresh snapshot only)
//     and every decoded barcode is displayed and appended;
//   - a held M15 does not re-trigger, and the next cycle uses a new requestId;
//   - a cycle that decodes nothing says so instead of re-showing an older code;
//   - a missing scan service reports a visible, actionable failure;
//   - the 手动 page's M15 拍照结束 test-signal button drives the same path.

#include <QtTest>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "adapters/barcode/barcode_reader_sdk_source.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "ui/MainWindow.h"
#include "ui/pages/manual_control_page.h"
#include "ui/pages/overview_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

constexpr quint16 kM15 = 15; // 扫码结束 (coil, read in the home/scan block)

// A scripted stand-in for the vendor DLL, shared by every case: each cycle gets
// its own accepted→completed exchange, and a case can replace the script to
// exercise a failure.
class FakeScanSdk : public IBarcodeSdk
{
public:
    BarcodeSdkReply status() override
    {
        if (!available) {
            return {BarcodeSdkStatus::LibraryUnavailable, QByteArray()};
        }
        ++statusCalls;
        return {BarcodeSdkStatus::Ok,
                QByteArray(R"({"ok":true,"code":"status","running":true,)"
                           R"("serverId":"server-1","version":1})")};
    }

    BarcodeSdkReply trigger(const QString &requestId) override
    {
        if (!available) {
            return {BarcodeSdkStatus::LibraryUnavailable, QByteArray()};
        }
        triggerIds.append(requestId);
        return {BarcodeSdkStatus::Ok,
                QStringLiteral(R"({"ok":true,"code":"accepted","state":"pending",)"
                               R"("requestId":"%1","serverId":"server-1"})")
                    .arg(requestId)
                    .toUtf8()};
    }

    BarcodeSdkReply result(const QString &requestId) override
    {
        if (!available) {
            return {BarcodeSdkStatus::LibraryUnavailable, QByteArray()};
        }
        // A test can park the cycle here to hold one open across a second M15
        // edge. The wait is bounded so a failing assertion can never leave the
        // worker thread blocked forever (which would hang stop()).
        if (holdResults) {
            QSemaphore *gate = holdResults;
            gate->tryAcquire(1, 5000);
        }
        resultIds.append(requestId);
        return {BarcodeSdkStatus::Ok, completedJson(requestId)};
    }

    // What the next completed cycle reports, in table order. Empty strings are
    // positions the SDK looked at and did not recognise.
    QStringList nextRows;
    bool available = true;
    // When set, result() waits on this semaphore before answering.
    QSemaphore *holdResults = nullptr;
    int statusCalls = 0;
    QStringList triggerIds;
    QStringList resultIds;

private:
    QByteArray completedJson(const QString &requestId) const
    {
        QStringList rows;
        int sequence = 1;
        for (const QString &barcode : nextRows) {
            rows.append(
                QStringLiteral(R"({"barcode":"%1","cameraId":"camera-window-2",)"
                               R"("format":"DataMatrix","rectId":%2,"sequence":%2})")
                    .arg(barcode)
                    .arg(sequence));
            ++sequence;
        }
        return QStringLiteral(R"({"ok":true,"code":"completed","state":"completed",)"
                              R"("requestId":"%1","serverId":"server-1","saved":true,)"
                              R"("decodedCount":%2,"message":"","rows":[%3]})")
            .arg(requestId)
            .arg(nextRows.size())
            .arg(rows.join(QLatin1Char(',')))
            .toUtf8();
    }
};

struct StartedApp
{
    QTemporaryDir dir;
    FakeScanSdk sdk;
    AppConfig cfg;
    std::unique_ptr<Application> app;
    SimulatedPlcGateway *gw = nullptr;
    OverviewPage *overview = nullptr;
    UsersSettingsPage *settings = nullptr;

    StartedApp()
    {
        cfg.useSimulatedGateway = true;
        cfg.simulatedTickIntervalMs = 0; // the test owns the clock
        cfg.databasePath = dir.filePath(QStringLiteral("app.db"));
        // Injected, caller-owned: the real adapter is never constructed, so no
        // vendor DLL is needed on the Linux dev loop.
        cfg.barcodeSource = nullptr; // set in start() so the adapter uses it
    }

    void start()
    {
        // A restart (persistedPathIsRestoredAtStartup) must not leak the
        // previous adapter, and the new one must be stopped before it dies.
        if (source != nullptr) {
            source->stop();
            delete source;
            source = nullptr;
        }
        // The adapter is constructed here with the fake SDK and injected into
        // the composition root, which then never builds its own.
        source = new BarcodeReaderSdkSource(&sdk);
        cfg.barcodeSource = source;
        app = std::make_unique<Application>(cfg);
        app->start();
        gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
        overview = app->window()->findChild<OverviewPage *>();
        settings = app->window()->findChild<UsersSettingsPage *>();
    }

    ~StartedApp()
    {
        // ORDER MATTERS: Application holds this source and touches it in
        // shutdown(), so the Application must be gone before the adapter is.
        // Application neither deletes nor reparents an injected source, so the
        // rig owns it.
        if (app) {
            app->shutdown();
            app.reset();
        }
        if (source != nullptr) {
            source->stop();
            delete source;
            source = nullptr;
        }
    }

    BarcodeReaderSdkSource *source = nullptr;

    void shutdown()
    {
        if (app)
            app->shutdown();
    }

    void advanceUntilOnline(int maxTicks = 20)
    {
        for (int i = 0; i < maxTicks && gw != nullptr && !gw->isOnline(); ++i)
            gw->tick();
    }

    // Settings are admin-only, so the session must be a real one: create the
    // initial administrator through the database and log in through the
    // production login path (restricted mode would reject the path save).
    void loginAsAdmin()
    {
        if (!adminCreated) {
            QSignalSpy adminSpy(app->database(), &DatabaseService::initialAdminCreated);
            QVERIFY(QMetaObject::invokeMethod(
                app->database(), "createInitialAdmin", Qt::QueuedConnection,
                Q_ARG(QString, QStringLiteral("admin")),
                Q_ARG(QString, QStringLiteral("s3cret!"))));
            QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
            adminCreated = true;
        }

        QSignalSpy loginSpy(app->database(), &DatabaseService::loginResult);
        QVERIFY(QMetaObject::invokeMethod(
            app->database(), "login", Qt::QueuedConnection,
            Q_ARG(QString, QStringLiteral("admin")),
            Q_ARG(QString, QStringLiteral("s3cret!"))));
        QTRY_VERIFY_WITH_TIMEOUT(loginSpy.size() > 0, 5000);
        QVERIFY(loginSpy[0][0].value<LoginResult>().ok);
        QTRY_COMPARE_WITH_TIMEOUT(app->coordinator()->role(), Role::Admin, 5000);
    }

    bool adminCreated = false;

    // One scan cycle: the PLC raises M15 (scan finished), the snapshot feed sees
    // the rising edge, and the adapter runs one cycle on its own thread.
    void raiseScanComplete()
    {
        gw->model().writeCoil(kM15, true);
        gw->tick();
    }

    void clearScanComplete()
    {
        gw->model().writeCoil(kM15, false);
        gw->tick();
    }

    // Waits until the overview surface shows `text`, ticking the simulated PLC
    // so a pending cycle can also complete.
    bool waitForText(const QString &text, int timeoutMs = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < timeoutMs) {
            if (overview->barcodeText().contains(text))
                return true;
            gw->tick();
            QTest::qWait(20);
        }
        return overview->barcodeText().contains(text);
    }
};

// Saves the append path through the real page -> Application -> SQLite path and
// waits for the confirmed echo (never optimistic).
void savePath(StartedApp &rig, const QString &path)
{
    QSignalSpy savedSpy(rig.settings, &UsersSettingsPage::saveBarcodePathRequested);
    emit rig.settings->saveBarcodePathRequested(path);
    QCOMPARE(savedSpy.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(rig.settings->barcodePathEdit()->text(), path, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.settings->barcodePathStatusText().contains(QStringLiteral("已保存")),
        5000);
}

QStringList fileLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    return text.split(QLatin1String("\r\n"), Qt::SkipEmptyParts);
}

} // namespace

class BarcodeIngestionDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void unconfiguredPathStaysNotConfigured();
    void configuredPathIsSavedAndEchoed();
    void scanCycleShowsEveryDecodedBarcode();
    void heldScanCompleteDoesNotRetrigger();
    void cycleWithoutAnyBarcodeIsReportedAsSuch();
    void scanServiceDownReportsAnActionableFailure();
    void persistedPathIsRestoredAtStartup();
    void manualScanTriggerDrivesTheWholePath();
    void refusedOverlappingScanIsVisible();
};

void BarcodeIngestionDeveloperTest::unconfiguredPathStaysNotConfigured()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    QVERIFY(rig.overview != nullptr);

    // Fresh installation: no path configured, so the surface is the 未配置
    // placeholder and never claims a connection.
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("未配置")));
    QVERIFY(!rig.overview->barcodeText().contains(QStringLiteral("已连接")));
    QVERIFY(!rig.overview->barcodeText().contains(QStringLiteral("在线")));

    // An M15 cycle with no configured path reports 未配置 and never reaches the
    // SDK at all — nothing to store means nothing to scan.
    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("未配置")));
    QCOMPARE(rig.sdk.triggerIds.size(), 0);
    QCOMPARE(rig.sdk.statusCalls, 0);
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::configuredPathIsSavedAndEchoed()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("等待扫码")));
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::scanCycleShowsEveryDecodedBarcode()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);

    // A real cycle: six rectangles, one of them not recognised.
    rig.sdk.nextRows = {QStringLiteral("C3003090^M10^260224^002695"),
                        QStringLiteral("C3003100^M10^260224^002696"),
                        QStringLiteral("C3003090^M10^260224^002697"),
                        QStringLiteral("C3003100^M10^260224^002698"),
                        QStringLiteral("C3003090^M10^260224^002699"),
                        QString()};

    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("002695")));
    const QString text = rig.overview->barcodeText();
    // Every decoded barcode is on the surface, not just the first one.
    QVERIFY(text.contains(QStringLiteral("002696")));
    QVERIFY(text.contains(QStringLiteral("002699")));
    // The unrecognised position is disclosed, never filled in.
    QVERIFY(text.contains(QStringLiteral("另有 1 个位置未识别到条码")));
    QCOMPARE(rig.sdk.triggerIds.size(), 1);

    // Five decoded barcodes were appended, one CRLF line each.
    const QStringList lines = fileLines(path);
    QCOMPARE(lines.size(), 5);
    QCOMPARE(lines.at(0), QStringLiteral("C3003090^M10^260224^002695"));
    QCOMPARE(lines.at(4), QStringLiteral("C3003090^M10^260224^002699"));
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::heldScanCompleteDoesNotRetrigger()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);

    rig.sdk.nextRows = {QStringLiteral("C3003090^M10^260224^002701")};
    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("002701")));
    QCOMPARE(rig.sdk.triggerIds.size(), 1);

    // M15 stays high for several polls: the edge is already consumed, so no
    // second cycle may start.
    for (int i = 0; i < 5; ++i)
        rig.gw->tick();
    QTest::qWait(100);
    QCOMPARE(rig.sdk.triggerIds.size(), 1);

    // A new board: M15 falls and rises again, with a new requestId and a new
    // result on the surface and in the file.
    rig.clearScanComplete();
    rig.sdk.nextRows = {QStringLiteral("C3003100^M10^260224^002702")};
    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("002702")));
    QCOMPARE(rig.sdk.triggerIds.size(), 2);
    QVERIFY2(rig.sdk.triggerIds.at(0) != rig.sdk.triggerIds.at(1),
             "each scan cycle must use its own requestId");
    QVERIFY(!rig.overview->barcodeText().contains(QStringLiteral("002701")));
    QCOMPARE(fileLines(path).size(), 2);
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::cycleWithoutAnyBarcodeIsReportedAsSuch()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);

    rig.sdk.nextRows = {QStringLiteral("C3003090^M10^260224^002700")};
    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("002700")));

    // Next board: the SDK completed but decoded nothing. The previous board's
    // barcode must NOT be presented as this cycle's result.
    rig.clearScanComplete();
    rig.sdk.nextRows = {QString(), QString(), QString()};
    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("本轮未识别到条码")));
    QVERIFY(!rig.overview->barcodeText().contains(QStringLiteral("002700")));
    // Nothing decoded → nothing appended; the file keeps the first board only.
    QCOMPARE(fileLines(path).size(), 1);
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::scanServiceDownReportsAnActionableFailure()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);

    // The scan program is closed: the operator gets something to act on, not
    // silence and not a status code.
    rig.sdk.available = false;
    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("扫码服务未启动")));
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("读取失败")));
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::persistedPathIsRestoredAtStartup()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);
    rig.shutdown();

    // Restart against the same database: the persisted path must be restored,
    // so the operator does not see 未配置 for a path that is configured.
    rig.app.reset(); // the shutdown() in ~StartedApp guards on a live app
    rig.start();
    rig.advanceUntilOnline();
    QTRY_COMPARE_WITH_TIMEOUT(rig.settings->barcodePathEdit()->text(), path, 5000);
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("等待扫码")));
    QVERIFY(!rig.overview->barcodeText().contains(QStringLiteral("未配置")));

    // And a scan cycle still runs against that path.
    rig.sdk.nextRows = {QStringLiteral("C3003090^M10^260224^002703")};
    rig.raiseScanComplete();
    QVERIFY(rig.waitForText(QStringLiteral("002703")));
    QCOMPARE(fileLines(path).size(), 1);
    rig.shutdown();
}

// The 手动 page's M15 拍照结束 test-signal button (user decision 2026-09-22)
// injects the scan-complete signal, so the whole path can be driven from the
// bench while the real scan program is absent.
void BarcodeIngestionDeveloperTest::manualScanTriggerDrivesTheWholePath()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);
    rig.sdk.nextRows = {QStringLiteral("C3003090^M10^260224^002777")};

    auto *manual = rig.app->window()->findChild<ManualControlPage *>();
    QVERIFY2(manual != nullptr, "the 手动 page must exist");
    PermissionButton *scan = manual->simSignalButton(kM15);
    QVERIFY2(scan != nullptr, "the M15 拍照结束 test-signal button must exist");
    QTRY_VERIFY_WITH_TIMEOUT(scan->isEnabled(), 5000); // 仅管理员 + 在线

    // One click sends the same single pulse as the neighbouring-station test
    // signals; the next snapshot carries M15=1 and the cycle runs.
    scan->click();
    QVERIFY(rig.waitForText(QStringLiteral("002777")));
    QCOMPARE(rig.sdk.triggerIds.size(), 1);
    QCOMPARE(fileLines(path).size(), 1);
    rig.shutdown();
}

// A 扫码结束 edge that arrives while the previous cycle is still polling is
// REFUSED, not queued — and a refusal must never be silent (contract: no silent
// rejection). The first cycle's own result must still land afterwards.
void BarcodeIngestionDeveloperTest::refusedOverlappingScanIsVisible()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    savePath(rig, path);

    // Park the first cycle inside result() so the second M15 edge cannot start
    // one of its own.
    QSemaphore gate;
    rig.sdk.holdResults = &gate;
    rig.sdk.nextRows = {QStringLiteral("C3003090^M10^260224^002801")};
    rig.raiseScanComplete();

    // Let the cycle reach the parked call before the second edge.
    QTRY_VERIFY_WITH_TIMEOUT(rig.source->cycleInProgress(), 5000);

    // A second board's 扫码结束 while the first is still running.
    rig.clearScanComplete();
    rig.raiseScanComplete();
    const QString text = rig.overview->barcodeText();
    QVERIFY2(text.contains(QStringLiteral("上一轮扫码尚未结束")),
             qPrintable(text));
    // The refused edge did NOT start a second cycle.
    QCOMPARE(rig.sdk.triggerIds.size(), 1);

    // Release the first cycle: its own result still arrives and is displayed.
    gate.release(1);
    rig.sdk.holdResults = nullptr;
    QVERIFY(rig.waitForText(QStringLiteral("002801")));
    QCOMPARE(fileLines(path).size(), 1);
    rig.shutdown();
}

QTEST_MAIN(BarcodeIngestionDeveloperTest)
#include "barcode_ingestion_developer_test.moc"
