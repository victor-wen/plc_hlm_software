// Developer integration test: barcode result ingestion end to end
// (user decision 2026-09-22).
//
// The scanning program is external and automatic, so the HMI's whole job is:
//   1. watch the PLC's M15 扫码结束 coil,
//   2. on its rising edge read the result file at the configured path,
//   3. show the readback (or the reason there is none).
//
// This test drives the real composition root (Application) with the in-process
// PLC simulator and a real result file, so the wiring under test is the
// production one: settings load/save through the SQLite app_settings table, the
// M15 edge on the snapshot feed, the worker-thread file read, and the overview
// page rendering.
//
// Guarantees pinned here:
//   - with no configured path the surface stays 未配置 and no file is read;
//   - a confirmed path save is echoed and reaches the adapter;
//   - M15 rising reads the file exactly once (edge-triggered, and only from a
//     fresh snapshot) and the newest line is displayed;
//   - a second scan cycle whose file did not change reports 本轮未读到条码
//     instead of re-showing the previous board's barcode;
//   - a lost result file reports a visible failure, never silence;
//   - the 手动 page's M15 拍照结束 test-signal button drives the same path.

#include <QtTest>

#include <QApplication>
#include <QLineEdit>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

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

struct StartedApp
{
    QTemporaryDir dir;
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
    }

    void start()
    {
        app = std::make_unique<Application>(cfg);
        app->start();
        gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
        overview = app->window()->findChild<OverviewPage *>();
        settings = app->window()->findChild<UsersSettingsPage *>();
    }

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

    // One scan cycle: the PLC raises M15 (scan finished), the snapshot feed
    // sees the rising edge, and the adapter reads the file on its own thread.
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
};

QString writeResultFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    file.write(data);
    file.close();
    return path;
}

} // namespace

class BarcodeIngestionDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void unconfiguredPathStaysNotConfigured();
    void configuredPathIsSavedAndEchoed();
    void scanCycleShowsTheNewestLine();
    void unchangedFileIsReportedAsNotRead();
    void missingFileReportsAFailure();
    void persistedPathIsRestoredAtStartup();
    void manualScanTriggerDrivesTheWholePath();
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

    // An M15 cycle with no configured path reports 未配置 rather than a value.
    rig.raiseScanComplete();
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.overview->barcodeText().contains(QStringLiteral("未配置")), 5000);
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::configuredPathIsSavedAndEchoed()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    QVERIFY(!writeResultFile(path, "AAA^1\r\n").isEmpty());

    // The save goes through the real page -> Application -> SQLite path and is
    // echoed only once it is confirmed (never optimistic).
    QSignalSpy savedSpy(rig.settings, &UsersSettingsPage::saveBarcodePathRequested);
    emit rig.settings->saveBarcodePathRequested(path);
    QCOMPARE(savedSpy.count(), 1);

    QTRY_COMPARE_WITH_TIMEOUT(rig.settings->barcodePathEdit()->text(), path, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.settings->barcodePathStatusText().contains(QStringLiteral("已保存")),
        5000);
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("等待扫码")));
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::scanCycleShowsTheNewestLine()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    QVERIFY(!writeResultFile(
                 path,
                 "C3003090^M10^260224^002700\r\n"
                 "C3003100^M10^260224^002695\r\n")
                 .isEmpty());
    emit rig.settings->saveBarcodePathRequested(path);
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.settings->barcodePathStatusText().contains(QStringLiteral("已保存")),
        5000);

    // The PLC raises M15 扫码结束: the newest line is read and displayed.
    rig.raiseScanComplete();
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.overview->barcodeText().contains(QStringLiteral("C3003100^M10^260224^002695")),
        5000);

    // A held M15 must not re-read: the edge is what triggers, not the level.
    rig.gw->tick();
    rig.gw->tick();
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("C3003100")));

    // Next cycle with a new line: the display follows the file.
    QVERIFY(!writeResultFile(path, QByteArray()).isEmpty());
    rig.clearScanComplete();
    QVERIFY(!writeResultFile(
                 path,
                 "C3003090^M10^260224^002700\r\n"
                 "C3003100^M10^260224^002695\r\n"
                 "C3003090^M10^260224^002701\r\n")
                 .isEmpty());
    rig.raiseScanComplete();
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.overview->barcodeText().contains(QStringLiteral("C3003090^M10^260224^002701")),
        5000);
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::unchangedFileIsReportedAsNotRead()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    QVERIFY(!writeResultFile(path, "C3003090^M10^260224^002700\r\n").isEmpty());
    emit rig.settings->saveBarcodePathRequested(path);
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.settings->barcodePathStatusText().contains(QStringLiteral("已保存")),
        5000);

    rig.raiseScanComplete();
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.overview->barcodeText().contains(QStringLiteral("C3003090^M10^260224^002700")),
        5000);

    // Second cycle, the scanning program decoded nothing and left the previous
    // text in place: the HMI must NOT present the old barcode as this board's.
    rig.clearScanComplete();
    rig.raiseScanComplete();
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.overview->barcodeText().contains(QStringLiteral("本轮未读到条码")), 5000);
    QVERIFY(!rig.overview->barcodeText().contains(QStringLiteral("002700")));
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::missingFileReportsAFailure()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    // A path that does not exist converges to a visible failure, never to
    // silence and never to a stale value.
    const QString path = rig.dir.filePath(QStringLiteral("absent.txt"));
    emit rig.settings->saveBarcodePathRequested(path);
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.settings->barcodePathStatusText().contains(QStringLiteral("已保存")),
        5000);

    rig.raiseScanComplete();
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.overview->barcodeText().contains(QStringLiteral("读取失败")), 5000);
    rig.shutdown();
}

void BarcodeIngestionDeveloperTest::persistedPathIsRestoredAtStartup()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    QVERIFY(!writeResultFile(path, "AAA^1\r\n").isEmpty());
    emit rig.settings->saveBarcodePathRequested(path);
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.settings->barcodePathStatusText().contains(QStringLiteral("已保存")),
        5000);
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("等待扫码")));
    rig.shutdown();

    // Restart against the same database: the persisted path must be restored,
    // so the operator does not see 未配置 for a path that is configured.
    rig.app.reset();
    rig.start();
    rig.advanceUntilOnline();
    // The persisted settings load asynchronously on database-ready.
    QTRY_COMPARE_WITH_TIMEOUT(rig.settings->barcodePathEdit()->text(), path, 5000);
    QVERIFY(rig.overview->barcodeText().contains(QStringLiteral("等待扫码")));
    QVERIFY(!rig.overview->barcodeText().contains(QStringLiteral("未配置")));

    // And a scan cycle still reads that file.
    rig.raiseScanComplete();
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.overview->barcodeText().contains(QStringLiteral("AAA^1")), 5000);
    rig.shutdown();
}

// The 手动 page's M15 拍照结束 test-signal button (user decision 2026-09-22)
// injects the scan-complete signal, so the whole path can be driven from the
// bench while the external scanning program is absent.
void BarcodeIngestionDeveloperTest::manualScanTriggerDrivesTheWholePath()
{
    StartedApp rig;
    rig.start();
    rig.advanceUntilOnline();
    rig.loginAsAdmin();

    const QString path = rig.dir.filePath(QStringLiteral("Barcode.txt"));
    QVERIFY(!writeResultFile(path, "C3003090^M10^260224^002777\r\n").isEmpty());
    emit rig.settings->saveBarcodePathRequested(path);
    QTRY_VERIFY_WITH_TIMEOUT(
        rig.settings->barcodePathStatusText().contains(QStringLiteral("已保存")),
        5000);

    auto *manual = rig.app->window()->findChild<ManualControlPage *>();
    QVERIFY2(manual != nullptr, "the 手动 page must exist");
    PermissionButton *scan = manual->simSignalButton(kM15);
    QVERIFY2(scan != nullptr, "the M15 拍照结束 test-signal button must exist");
    QTRY_VERIFY_WITH_TIMEOUT(scan->isEnabled(), 5000); // 仅管理员 + 在线

    // One click sends the same single pulse as the neighbouring-station test
    // signals; the next snapshot carries M15=1, the rising edge reads the file.
    scan->click();
    for (int i = 0; i < 20
                    && !rig.overview->barcodeText().contains(QStringLiteral("002777"));
         ++i) {
        rig.gw->tick();
        QTest::qWait(20);
    }
    QVERIFY2(rig.overview->barcodeText().contains(QStringLiteral("002777")),
             qPrintable(rig.overview->barcodeText()));
    rig.shutdown();
}

QTEST_MAIN(BarcodeIngestionDeveloperTest)
#include "barcode_ingestion_developer_test.moc"