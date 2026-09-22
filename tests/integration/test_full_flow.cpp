// Task 20d full in-process integration tests (spec §15.4 stage 3).
//
// Exercises the real composition root pieces together: DatabaseService
// (async worker thread), SimulatedPlcGateway (deterministic tick), the
// ControlCoordinator (PulseTransport wired to the gateway), ShellModel,
// LifecycleController and MainWindow. No real PLC, no real waits: the
// gateway clock advances only via tick(), and DB results are awaited with
// QTRY_VERIFY_WITH_TIMEOUT (the queued worker-thread path).
//
// Coverage (spec §15.4):
//  1. First-run DB: ready -> needsInitialAdmin -> createInitialAdmin ->
//     login -> 3 bad logins lock -> session timeout -> role downgrade.
//  2. Full flow: reset -> recipe -> adjustWidth(300) -> setMode(true) ->
//     start -> stop, asserted via coordinator signals + snapshot state.
//  3. Adjust precondition failure, dynamic timeout (M45/M14/D110=10),
//     estop set/release with latched fault.
//  4. Link down/up, heartbeat freeze, illegal value (D204=0).
//  5. Alarm edges, audit append/list, retention cleanup, restart
//     persistence, restricted mode.
//  6. MainWindow 1920x1080 offscreen: 7 nav items, page switch clears
//     holds, Anonymous role disables action buttons with tooltip reasons.

#include <QtTest>
#include <QSignalSpy>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QFile>
#include <QMetaObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QListWidget>

#include "adapters/modbus/qt_modbus_plc_gateway.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "app/lifecycle_controller.h"
#include "application/control_coordinator.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/hold_button.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching AddressTable).
constexpr quint16 kM42 = 42;
constexpr quint16 kM50 = 50;
constexpr quint16 kM61 = 61; // 回原点完成 (mirrored to M9 in D100 bit9)
constexpr quint16 kM100 = 100;
constexpr quint16 kM101 = 101;
constexpr quint16 kM102 = 102;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kD128 = 128;
constexpr quint16 kD204 = 204;

// Drive a reset+home-return to a ready manual state via the raw gateway.
void homeReady(SimulatedPlcGateway &gw)
{
    // PLC-HMI-011 D6: the M103 pulse no longer starts homing; the HMI's single
    // sustained M50=1 home-start write does.
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.model().writeCoil(kM50, true);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

// Build a coordinator wired to the simulated gateway (same pattern as
// tests/unit/test_control_coordinator.cpp).
ControlCoordinator *makeCoordinator(SimulatedPlcGateway &gw, qint64 &now)
{
    ControlCoordinator::PulseTransport t;
    // Route every submission through the gateway's submit API so its
    // submission bookkeeping runs and the correlated submissionCompleted is
    // emitted on tick() (PLC-HMI-011: the reset only converges when both the
    // M103 pulse and the M50 home-start write have correlated completions).
    t.startPulse = [&gw](quint16 a) -> SubmissionResult { return gw.submitPulse(a); };
    t.writeHold = [&gw](quint16 a, bool v) -> SubmissionResult {
        return gw.submitWriteCoil(a, v);
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority p) -> SubmissionResult {
        return gw.submitWriteCoil(a, v, p);
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority p) -> SubmissionResult {
        return gw.submitWriteRegister(a, v, p);
    };
    auto *c = new ControlCoordinator(std::move(t), ControlCoordinator::Config(),
                                     [&now]() { return now; });
    QObject::connect(&gw, &SimulatedPlcGateway::snapshotReady, c,
                     [c](quint64, const DeviceSnapshot &s) { c->onSnapshot(s); });
    QObject::connect(&gw, &SimulatedPlcGateway::connectionStateChanged, c,
                     [c](quint64, bool online) { c->onConnectionChanged(online); });
    QObject::connect(&gw, &SimulatedPlcGateway::submissionCompleted, c,
                     [c](const SubmissionCompletion &completion) {
                         c->onSubmissionCompleted(completion);
                     });
    if (gw.hasSnapshot())
        c->onSnapshot(gw.lastSnapshot());
    return c;
}

// Sends a synthetic mouse press at the widget center (hold start).
void pressAt(QWidget *w)
{
    const QPoint center = w->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, center, w->mapToGlobal(center),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
}

} // namespace

class FullFlowTest : public QObject
{
    Q_OBJECT

private slots:
    // Production startup must not silently substitute an online simulator.
    void defaultConfigurationUsesRealGateway();
    void persistedSerialConfigAppliedBeforeRealGatewayStarts();
    // --- 1. first-run DB + auth + session + role downgrade -------------------
    void firstRunAdminLoginLockoutSessionTimeout();
    // --- 2. full control flow ------------------------------------------------
    void fullFlowResetAdjustAutoStartStop();
    // --- 3. adjust precondition / dynamic timeout / estop latch --------------
    void adjustPreconditionFailure();
    void dynamicTimeoutFault10();
    void estopSetReleaseLatchesFault();
    // --- 4. link / heartbeat / illegal value ---------------------------------
    void linkDownRejectsWritesAndRecovers();
    void heartbeatFreezeGoesOfflineAndRecovers();
    void illegalValueMarksFieldInvalid();
    // --- 5. alarms / audit / retention / restart / restricted ----------------
    void alarmEdgesAndAudit();
    void retentionCleanupPurgesOldRows();
    void restartPersistsUserAndRecipe();
    void restrictedModeAllowsOnlyStopAndEstop();
    // --- 6. MainWindow offscreen ---------------------------------------------
    void mainWindowOffscreenShell();
    // --- 7. composition root (Application) adjustWidth convergence ------------
    void applicationAdjustWidthConverges();
};

void FullFlowTest::defaultConfigurationUsesRealGateway()
{
    const AppConfig cfg;
    QVERIFY(!cfg.useSimulatedGateway);
}

// --- 1. first-run DB + auth + session + role downgrade -----------------------

void FullFlowTest::persistedSerialConfigAppliedBeforeRealGatewayStarts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("app.db"));

    DatabaseService seed(databasePath);
    QSignalSpy readySpy(&seed, &DatabaseService::ready);
    seed.start();
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 5000);

    const QVector<QPair<QString, QString>> settings{
        {QStringLiteral("serial.comPort"), QStringLiteral("COM247")},
        {QStringLiteral("serial.station"), QStringLiteral("7")},
        {QStringLiteral("serial.baudRate"), QStringLiteral("19200")},
        {QStringLiteral("serial.stopBits"), QStringLiteral("2")},
        {QStringLiteral("serial.parity"), QStringLiteral("偶")},
        {QStringLiteral("serial.timeoutMs"), QStringLiteral("750")},
        {QStringLiteral("serial.readRetries"), QStringLiteral("3")},
    };
    QSignalSpy savedSpy(&seed, &DatabaseService::settingSaved);
    for (const auto &entry : settings) {
        SettingRecord setting;
        setting.key = entry.first;
        setting.typedValue = entry.second;
        setting.updatedBy = QStringLiteral("test");
        seed.setSetting(setting);
    }
    QTRY_COMPARE_WITH_TIMEOUT(savedSpy.count(), int(settings.size()), 5000);
    seed.stop();

    AppConfig cfg;
    cfg.databasePath = databasePath;
    cfg.useSimulatedGateway = false;
    Application app(cfg);
    IPlcGateway *initialGateway = app.gateway();
    app.start();

    QTRY_VERIFY_WITH_TIMEOUT(app.gateway() != initialGateway, 5000);
    auto *gateway = qobject_cast<QtModbusPlcGateway *>(app.gateway());
    QVERIFY(gateway != nullptr);
    const QtModbusPlcGateway::Config applied = gateway->configuration();
    QCOMPARE(applied.portName, QStringLiteral("COM247"));
    QCOMPARE(applied.station, quint8(7));
    QCOMPARE(applied.baudRate, 19200);
    QCOMPARE(applied.stopBits, QSerialPort::TwoStop);
    QCOMPARE(applied.parity, QSerialPort::EvenParity);
    QCOMPARE(applied.timeoutMs, 750);
    QCOMPARE(applied.readRetries, 3);
    app.shutdown();
}

void FullFlowTest::firstRunAdminLoginLockoutSessionTimeout()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService db(dir.filePath(QStringLiteral("app.db")));
    QSignalSpy readySpy(&db, &DatabaseService::ready);
    db.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);
    QVERIFY(!db.isRestricted());

    // First start: no user -> needsInitialAdmin true.
    QSignalSpy needsSpy(&db, &DatabaseService::initialAdminNeeded);
    QVERIFY(QMetaObject::invokeMethod(&db, "needsInitialAdmin", Qt::QueuedConnection));
    QTRY_VERIFY_WITH_TIMEOUT(needsSpy.size() > 0, 5000);
    QCOMPARE(needsSpy[0][0].toBool(), true);

    // Create the initial admin, then login succeeds.
    QSignalSpy adminSpy(&db, &DatabaseService::initialAdminCreated);
    QVERIFY(QMetaObject::invokeMethod(
        &db, "createInitialAdmin", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
    QCOMPARE(adminSpy[0][0].toBool(), true);

    QSignalSpy loginSpy(&db, &DatabaseService::loginResult);
    QVERIFY(QMetaObject::invokeMethod(
        &db, "login", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.size() > 0, 5000);
    QVERIFY(loginSpy[0][0].value<LoginResult>().ok);

    // 3 consecutive bad logins lock the account (spec §11.5).
    for (int i = 0; i < 3; ++i) {
        QSignalSpy badSpy(&db, &DatabaseService::loginResult);
        QVERIFY(QMetaObject::invokeMethod(
            &db, "login", Qt::QueuedConnection,
            Q_ARG(QString, QStringLiteral("admin")),
            Q_ARG(QString, QStringLiteral("wrong"))));
        QTRY_VERIFY_WITH_TIMEOUT(badSpy.size() > 0, 5000);
        QCOMPARE(badSpy[0][0].value<LoginResult>().ok, false);
        if (i == 2)
            QCOMPARE(badSpy[0][0].value<LoginResult>().reason,
                     QStringLiteral("locked"));
    }
    db.stop();

    // --- session timeout + role downgrade (spec §11.5) ------------------------
    ShellModel shell;
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> coord(makeCoordinator(gw, now));
    MainWindow window;
    window.show();
    auto *usersPage = window.findChild<UsersSettingsPage *>();
    QVERIFY(usersPage != nullptr);
    LifecycleController lc(&shell, coord.get(), &window, usersPage,
                           [](const QString &, const QString &, const QString &,
                              AuditResult, const QString &) {},
                           []() {});
    connect(usersPage, &UsersSettingsPage::logoutRequested, &lc,
            &LifecycleController::onLogoutRequested);
    connect(usersPage, &UsersSettingsPage::logoutClearRequested, &lc,
            &LifecycleController::onLogoutClearRequested);

    UserRecord admin;
    admin.username = QStringLiteral("admin");
    admin.role = Role::Admin;
    lc.onLoginSucceeded(admin);
    QCOMPARE(coord->role(), Role::Admin);
    QCOMPARE(shell.role(), Role::Admin);

    // Drive the countdown deterministically. Real user input resets an
    // authenticated session; the next three idle ticks then expire it.
    lc.setSessionTimeoutSec(3);
    lc.startSessionTimer();
    lc.onSessionTick();
    QCOMPARE(lc.sessionRemainingSec(), 2);
    QKeyEvent activity(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QApplication::sendEvent(&window, &activity);
    QCOMPARE(lc.sessionRemainingSec(), 3);
    lc.onSessionTick();
    lc.onSessionTick();
    lc.onSessionTick();
    QCOMPARE(lc.sessionRemainingSec(), 0);
    // Expiry logged the user out: role downgraded to Anonymous.
    QCOMPARE(coord->role(), Role::Anonymous);
    QCOMPARE(shell.role(), Role::Anonymous);
    // Anonymous cannot reset (permission downgrade blocks the command).
    QVERIFY(!coord->reset().accepted);
}

// --- 2. full control flow ----------------------------------------------------

void FullFlowTest::fullFlowResetAdjustAutoStartStop()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    QList<Command> accepted;
    QList<QPair<Command, bool>> results;
    connect(c.get(), &ControlCoordinator::commandAccepted, this,
            [&accepted](Command cmd) { accepted.append(cmd); });
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&results](Command cmd, bool ok, const QString &) {
                results.append({cmd, ok});
            });

    // 复位 (M103 pulse only, user decision 2026-09-21) -> converges on the
    // fixed 200 ms boundary (PLC-HMI-010 D1/D3); no M50/M61 readback
    // participates in the result, and the reset must NOT write M50.
    QVERIFY(c->reset().accepted);
    gw.tick(); // M103 pulse completion
    QVERIFY(!gw.model().readCoil(kM50)); // the reset no longer starts homing
    QVERIFY(c->resetInProgress());
    now += ControlCoordinator::kResetCompletionDelayMs;
    gw.tick();
    QVERIFY(accepted.contains(Command::Reset));
    QVERIFY(results.contains({Command::Reset, true}));

    // 回原点 (one sustained M50=1 write, user decision 2026-09-21): the PLC
    // starts homing, clears M50 itself and raises M61/M9 when it completes.
    // The width adjust below still needs a homed machine on the PLC side.
    QVERIFY(c->homeStart().accepted);
    QVERIFY(c->homeStartInProgress());
    gw.tick();
    QVERIFY(gw.model().readCoil(kM50)); // the home-start write reached the PLC
    gw.tick(); // the M50 readback confirms the write (回原点已启动)
    gw.tick(); // home return takes 2 s: the PLC clears M50 and sets M61
    QVERIFY(!c->homeStartInProgress());
    QVERIFY(!gw.model().readCoil(kM50));
    QVERIFY(gw.model().readCoil(kM61));
    QVERIFY(results.contains({Command::HomeStart, true}));

    // 配方调宽 300: D128 written, M43 pulse, converges on M44 + D130=300.
    // D204 is pinned to 1280 so the pulse-based run keeps the documented 7 s
    // duration: ceil(100 * 1280 / (15 * 1280)) = 7 s (PLC-HMI-005 D1/D2).
    //
    // The decoded SBR_HOME completion zeroed D130 (`DMOV K0 D130`), so the run
    // is first brought back to the neutral target 200 — the 100-count run the
    // 7 s figure describes. It also restores 自动准备完成, which the decoded
    // M60 rung ties to D128 == D130 (user decision 2026-09-22).
    gw.model().writeCoil(43, true);
    gw.model().writeCoil(43, false);
    gw.tick();
    gw.tick(); // ceil(200 * 128 / 19200) = 2 s
    QCOMPARE(gw.lastSnapshot().currentWidth(), quint16(200));

    gw.model().writeRegister(kD204, 1280);
    QVERIFY(c->adjustWidth(300).accepted);
    QCOMPARE(gw.model().readRegister(kD128), quint16(300));
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34()); // adjusting
    for (int i = 0; i < 7; ++i) // ceil(100 * 1280 / 19200) = 7 s
        gw.tick();
    QVERIFY(gw.lastSnapshot().m44());
    QVERIFY(!gw.lastSnapshot().m45());
    QCOMPARE(gw.lastSnapshot().currentWidth(), quint16(300));
    QVERIFY(accepted.contains(Command::AdjustWidth));
    QVERIFY(results.contains({Command::AdjustWidth, true}));

    // 自动模式 (M104=1 -> M2=1).
    QVERIFY(c->setMode(true).accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());
    QVERIFY(!gw.lastSnapshot().m1());
    QVERIFY(results.contains({Command::ModeSwitch, true}));

    // 启动 (M101 pulse -> M3=1).
    QVERIFY(c->start().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m3());
    QVERIFY(results.contains({Command::Start, true}));

    // 停止 (M102 pulse -> M3=0).
    QVERIFY(c->stop().accepted);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m3());
    QVERIFY(results.contains({Command::Stop, true}));
}

// --- 3. adjust precondition / dynamic timeout / estop latch -------------------

void FullFlowTest::adjustPreconditionFailure()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);

    // User decision 2026-09-21: an un-homed machine is no longer an HMI adjust
    // gate, so the remaining precondition exercised here is the automatic mode
    // (M1 manual required). The command must still be rejected visibly and send
    // no M43 pulse.
    QVERIFY(c->setMode(true).accepted); // M104=1 -> M2=1, M1=0
    gw.tick();
    QVERIFY(gw.lastSnapshot().m2());

    QSignalSpy rejectedSpy(c.get(), &ControlCoordinator::commandRejected);
    const ControlCoordinator::CommandResult r = c->adjustWidth(300);
    QVERIFY(!r.accepted);
    QVERIFY(!r.reason.isEmpty());
    QCOMPARE(rejectedSpy.size(), 1);
    QCOMPARE(rejectedSpy[0][0].value<Command>(), Command::AdjustWidth);
    QVERIFY(!gw.model().readCoil(43)); // no M43 pulse sent
}

void FullFlowTest::dynamicTimeoutFault10()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    // Stall the motor: positioning never completes -> fixed 30 s timeout.
    gw.model().setPositioningStall(true);
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34());

    // Fixed T6 K300 = 30 s (PLC-HMI-005 D2).
    for (int i = 0; i < 30; ++i)
        gw.tick();
    QVERIFY(!gw.lastSnapshot().m34());
    QVERIFY(!gw.lastSnapshot().m44());
    QVERIFY(gw.lastSnapshot().m45());
    QVERIFY(gw.lastSnapshot().m14()); // latched fault
    QCOMPARE(gw.lastSnapshot().faultCode(), quint16(10)); // D110=10
    QVERIFY(!c->adjustInProgress());
}

void FullFlowTest::estopSetReleaseLatchesFault()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Admin);
    homeReady(gw);

    // Software estop set: M0=1, latched fault M14=1, D110=1.
    QVERIFY(c->estopSet().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m14());
    QCOMPARE(gw.lastSnapshot().faultCode(), quint16(1));

    // Release (admin): M0=0 but the fault stays latched until reset.
    QVERIFY(c->estopRelease().accepted);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().m0());
    QVERIFY(gw.lastSnapshot().m14());
    QCOMPARE(gw.lastSnapshot().faultCode(), quint16(1));

    // Reset (M103 pulse only, user decision 2026-09-21): the PLC clears the
    // latched fault and the reset converges on the HMI's fixed 200 ms boundary
    // once the pulse completion arrived (PLC-HMI-010 D3). Homing is the
    // separate 回原点 command and is not part of the reset.
    QVERIFY(c->reset().accepted);
    gw.tick(); // M103 pulse completion
    QVERIFY(!gw.model().readCoil(kM50)); // the reset no longer starts homing
    now += ControlCoordinator::kResetCompletionDelayMs;
    gw.tick();
    QVERIFY(!c->resetInProgress());
    QVERIFY(!gw.lastSnapshot().m14());
    QCOMPARE(gw.lastSnapshot().faultCode(), quint16(0));
}

// --- 4. link / heartbeat / illegal value --------------------------------------

void FullFlowTest::linkDownRejectsWritesAndRecovers()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    c->setRole(Role::Anonymous);

    QSignalSpy connSpy(&gw, &SimulatedPlcGateway::connectionStateChanged);
    QSignalSpy completionSpy(&gw, &SimulatedPlcGateway::submissionCompleted);

    gw.setLinkDown(true);
    QVERIFY(!gw.isOnline());
    QVERIFY(!c->online());
    QCOMPARE(connSpy.size(), 1);
    QCOMPARE(connSpy[0][1].toBool(), false); // (generation, online)

    // Submissions rejected synchronously while offline, not applied and not
    // completed (contract: rejected submissions emit no completion).
    const SubmissionResult rejected = gw.submitWriteCoil(kM100, true);
    QVERIFY(!rejected.accepted);
    QVERIFY(!rejected.immediate_rejection_reason.isEmpty());
    QCOMPARE(completionSpy.size(), 0);
    QVERIFY(!gw.model().readCoil(kM100));

    // Restore: still offline until the next tick reconnects.
    gw.setLinkDown(false);
    QVERIFY(!gw.isOnline());
    gw.tick();
    QVERIFY(gw.isOnline());
    QVERIFY(c->online());
    QCOMPARE(connSpy.size(), 2);
    QCOMPARE(connSpy[1][1].toBool(), true);

    // Submissions accepted again and completed with a readback confirmation.
    const SubmissionResult accepted = gw.submitWriteCoil(kM100, true);
    QVERIFY(accepted.accepted);
    gw.tick();
    QCOMPARE(completionSpy.size(), 1);
    QCOMPARE(completionSpy[0][0].value<SubmissionCompletion>().request_id,
             accepted.request_id);
    QVERIFY(completionSpy[0][0].value<SubmissionCompletion>().result);
    QVERIFY(gw.model().readCoil(kM100));
}

void FullFlowTest::heartbeatFreezeGoesOfflineAndRecovers()
{
    SimulatedPlcGateway gw;
    gw.start();
    gw.tick();
    QVERIFY(gw.isOnline());

    // Freeze the heartbeat: D140 unchanged; offline after 3 ticks (spec §8.4).
    gw.setHeartbeatFrozen(true);
    const quint16 hb = gw.lastSnapshot().heartbeat();
    gw.tick();
    gw.tick();
    QCOMPARE(gw.lastSnapshot().heartbeat(), hb);
    QVERIFY(gw.isOnline()); // not yet past the threshold
    gw.tick();
    QVERIFY(!gw.isOnline());

    // Unfreeze: the next tick reconnects with a fresh snapshot.
    gw.setHeartbeatFrozen(false);
    gw.tick();
    QVERIFY(gw.isOnline());
    QVERIFY(gw.lastSnapshot().heartbeat() != hb); // D140 moving again
}

void FullFlowTest::illegalValueMarksFieldInvalid()
{
    SimulatedPlcGateway gw;
    gw.start();
    // D204=0 is out of range (1-32767): the snapshot must mark the field
    // invalid (spec §9).
    gw.model().writeRegister(kD204, 0);
    gw.tick();
    QVERIFY(!gw.lastSnapshot().fieldValid(SnapshotField::PulsePerMm));
    QCOMPARE(gw.lastSnapshot().pulsePerMm(), quint16(0));
}

// --- 5. alarms / audit / retention / restart / restricted ---------------------

void FullFlowTest::alarmEdgesAndAudit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService db(dir.filePath(QStringLiteral("app.db")));
    QSignalSpy readySpy(&db, &DatabaseService::ready);
    db.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);

    // First snapshot adopts state (no edge); then d110=1 starts an active
    // alarm, d110=0 ends it (spec §12).
    QSignalSpy procSpy(&db, &DatabaseService::alarmSnapshotProcessed);
    db.feedPlcAlarmSnapshot(0, false, false, 1);
    QTRY_VERIFY_WITH_TIMEOUT(procSpy.size() == 1, 5000);
    db.feedPlcAlarmSnapshot(1, true, false, 2);
    QTRY_VERIFY_WITH_TIMEOUT(procSpy.size() == 2, 5000);

    QSignalSpy alarmsSpy(&db, &DatabaseService::recentAlarmsLoaded);
    db.listRecentAlarms(10);
    QTRY_VERIFY_WITH_TIMEOUT(alarmsSpy.size() > 0, 5000);
    const QVector<AlarmEventRecord> active = alarmsSpy[0][0].value<QVector<AlarmEventRecord>>();
    QCOMPARE(active.size(), 1);
    QVERIFY(active[0].isActive());
    QCOMPARE(active[0].code, quint16(1));

    // Clear edge: d110=0 with M14/M4 clear ends the event.
    db.feedPlcAlarmSnapshot(0, false, false, 3);
    QTRY_VERIFY_WITH_TIMEOUT(procSpy.size() == 3, 5000);
    QSignalSpy endedSpy(&db, &DatabaseService::recentAlarmsLoaded);
    db.listRecentAlarms(10);
    QTRY_VERIFY_WITH_TIMEOUT(endedSpy.size() > 0, 5000);
    const QVector<AlarmEventRecord> ended = endedSpy[0][0].value<QVector<AlarmEventRecord>>();
    QCOMPARE(ended.size(), 1);
    QVERIFY(!ended[0].isActive());

    // Audit append + list.
    AuditRecord a;
    a.occurredAt = QDateTime::currentDateTimeUtc();
    a.username = QStringLiteral("admin");
    a.role = Role::Admin;
    a.action = QStringLiteral("test.action");
    a.target = QStringLiteral("M3");
    a.result = AuditResult::Success;
    QSignalSpy auditSpy(&db, &DatabaseService::auditAppended);
    db.appendAudit(a);
    QTRY_VERIFY_WITH_TIMEOUT(auditSpy.size() > 0, 5000);
    QCOMPARE(auditSpy[0][0].toBool(), true);

    QSignalSpy listSpy(&db, &DatabaseService::recentAuditLoaded);
    db.listRecentAudit(10);
    QTRY_VERIFY_WITH_TIMEOUT(listSpy.size() > 0, 5000);
    const QVector<AuditRecord> records = listSpy[0][0].value<QVector<AuditRecord>>();
    QVERIFY(records.size() >= 1);
    QCOMPARE(records[0].action, QStringLiteral("test.action"));
    db.stop();
}

void FullFlowTest::retentionCleanupPurgesOldRows()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("app.db"));
    DatabaseService db(dbPath);
    QSignalSpy readySpy(&db, &DatabaseService::ready);
    db.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);

    // Insert an ended alarm and an audit row older than 365 days via a second
    // connection (WAL allows concurrent access).
    {
        QSqlDatabase conn = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), QStringLiteral("retention_test"));
        conn.setDatabaseName(dbPath);
        QVERIFY(conn.open());
        QSqlQuery q(conn);
        const QString old = QDateTime::currentDateTimeUtc()
                                .addDays(-400)
                                .toString(Qt::ISODate);
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO alarm_events (source, code, message_snapshot, severity,"
            " started_at, ended_at, snapshot_sequence)"
            " VALUES ('plc', 1, 'x', 'critical', '%1', '%1', 1)").arg(old)));
        QVERIFY(q.exec(QStringLiteral(
            "INSERT INTO audit_log (occurred_at, username, role, action, target,"
            " redacted_parameters, result, reason)"
            " VALUES ('%1', 'admin', 'admin', 'old', 'x', '', 'success', '')").arg(old)));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("retention_test"));

    // Retention cleanup purges both old rows (spec §12: 365 days).
    QSignalSpy cleanupSpy(&db, &DatabaseService::retentionCleanupDone);
    db.runRetentionCleanup();
    QTRY_VERIFY_WITH_TIMEOUT(cleanupSpy.size() > 0, 5000);
    QCOMPARE(cleanupSpy[0][0].toLongLong(), qint64(1)); // removedAlarms
    QCOMPARE(cleanupSpy[0][1].toLongLong(), qint64(1)); // removedAudit

    QSignalSpy alarmsSpy(&db, &DatabaseService::recentAlarmsLoaded);
    db.listRecentAlarms(10);
    QTRY_VERIFY_WITH_TIMEOUT(alarmsSpy.size() > 0, 5000);
    QCOMPARE(alarmsSpy[0][0].value<QVector<AlarmEventRecord>>().size(), 0);

    QSignalSpy auditSpy(&db, &DatabaseService::recentAuditLoaded);
    db.listRecentAudit(10);
    QTRY_VERIFY_WITH_TIMEOUT(auditSpy.size() > 0, 5000);
    QCOMPARE(auditSpy[0][0].value<QVector<AuditRecord>>().size(), 0);
    db.stop();
}

void FullFlowTest::restartPersistsUserAndRecipe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("app.db"));

    {
        DatabaseService db(dbPath);
        QSignalSpy readySpy(&db, &DatabaseService::ready);
        db.start();
        QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);

        QSignalSpy adminSpy(&db, &DatabaseService::initialAdminCreated);
        QVERIFY(QMetaObject::invokeMethod(
            &db, "createInitialAdmin", Qt::QueuedConnection,
            Q_ARG(QString, QStringLiteral("admin")),
            Q_ARG(QString, QStringLiteral("s3cret!"))));
        QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
        QCOMPARE(adminSpy[0][0].toBool(), true);

        RecipeRecord r;
        r.name = QStringLiteral("宽300");
        r.targetWidthRaw = 300;
        r.createdBy = QStringLiteral("admin");
        r.updatedBy = QStringLiteral("admin");
        QSignalSpy recipeSpy(&db, &DatabaseService::recipeSaved);
        // Direct call: the service lives on the worker thread, so Qt queues
        // the invocation automatically (the production wiring path).
        db.saveRecipe(r);
        QTRY_VERIFY_WITH_TIMEOUT(recipeSpy.size() > 0, 5000);
        QCOMPARE(recipeSpy[0][0].toBool(), true);
        db.stop();
    }

    // New service on the same path: user + recipe still there.
    DatabaseService db2(dbPath);
    QSignalSpy readySpy2(&db2, &DatabaseService::ready);
    db2.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy2.size() > 0, 5000);

    QSignalSpy needsSpy(&db2, &DatabaseService::initialAdminNeeded);
    QVERIFY(QMetaObject::invokeMethod(&db2, "needsInitialAdmin", Qt::QueuedConnection));
    QTRY_VERIFY_WITH_TIMEOUT(needsSpy.size() > 0, 5000);
    QCOMPARE(needsSpy[0][0].toBool(), false); // admin persisted

    QSignalSpy recipesSpy(&db2, &DatabaseService::recipesLoaded);
    QVERIFY(QMetaObject::invokeMethod(&db2, "listRecipes", Qt::QueuedConnection));
    QTRY_VERIFY_WITH_TIMEOUT(recipesSpy.size() > 0, 5000);
    const QVector<RecipeRecord> recipes = recipesSpy[0][0].value<QVector<RecipeRecord>>();
    QCOMPARE(recipes.size(), 1);
    QCOMPARE(recipes[0].name, QStringLiteral("宽300"));
    QCOMPARE(recipes[0].targetWidthRaw, 300);
    db2.stop();
}

void FullFlowTest::restrictedModeAllowsOnlyStopAndEstop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Corrupt the database file: SQLite cannot open it -> restricted mode.
    const QString dbPath = dir.filePath(QStringLiteral("app.db"));
    {
        QFile f(dbPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not a sqlite database file at all");
    }
    DatabaseService db(dbPath);
    QSignalSpy restrictedSpy(&db, &DatabaseService::databaseRestricted);
    db.start();
    QTRY_VERIFY_WITH_TIMEOUT(restrictedSpy.size() > 0, 5000);
    QVERIFY(db.isRestricted());
    db.stop();

    // Restricted mode: only online-stop and software-estop stay (spec §13).
    LifecycleController lc(nullptr, nullptr, nullptr, nullptr,
                           [](const QString &, const QString &, const QString &,
                              AuditResult, const QString &) {},
                           []() {});
    lc.enterRestrictedMode(restrictedSpy[0][0].toString());
    QVERIFY(lc.restricted());
    QVERIFY(lc.commandAllowed(Command::Stop));
    QVERIFY(lc.commandAllowed(Command::EstopSet));
    QVERIFY(!lc.commandAllowed(Command::Reset));
    QVERIFY(!lc.commandAllowed(Command::Start));
    QVERIFY(!lc.commandAllowed(Command::AdjustWidth));
}

// --- 6. MainWindow offscreen --------------------------------------------------

void FullFlowTest::mainWindowOffscreenShell()
{
    MainWindow w;
    w.resize(1920, 1080);
    w.show();
    QApplication::processEvents();

    // 7 nav items (spec §11.1).
    QCOMPARE(w.navItemCount(), 7);

    // Page switch clears hold intents (spec §10.7).
    HoldButton held(QStringLiteral("点动"));
    w.registerHoldWidget(&held);
    pressAt(&held);
    QVERIFY(w.hasActiveHolds());
    w.setCurrentPage(2);
    QVERIFY(!w.hasActiveHolds());
    QVERIFY(!held.isHeld());

    // Anonymous role: action buttons disabled with a reason that is both a
    // tooltip supplement and inline visible text, never tooltip-only
    // (spec §11.4; PLC-HMI-008 D3, forbidden_change 693).
    ActionBar *bar = w.findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    QVERIFY(!w.startButton()->isEnabled());
    QVERIFY(!w.startButton()->toolTip().isEmpty());
    QVERIFY2(!bar->startButton()->visibleReasonText().isEmpty(),
             "a disabled action-bar control must show its reason inline");
    QVERIFY(!w.resetButton()->isEnabled());
    QVERIFY(!w.resetButton()->toolTip().isEmpty());
    QVERIFY2(!bar->resetButton()->visibleReasonText().isEmpty(),
             "a disabled action-bar control must show its reason inline");
    QVERIFY(!w.estopButton()->isEnabled());
    QVERIFY(!w.estopButton()->toolTip().isEmpty());
    QVERIFY2(!bar->estopButton()->visibleReasonText().isEmpty(),
             "a disabled software-estop control must show its reason inline");
}

// --- 7. composition root (Application) adjustWidth convergence ----------------
//
// Regression test for the Task 20 review finding: Application::wireGateway
// must feed correlated completions into ControlCoordinator, or the adjust
// width flow can never converge. This test drives the real composition root
// (Application), not a hand-rolled coordinator, so the wiring is exercised.

void FullFlowTest::applicationAdjustWidthConverges()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AppConfig cfg;
    cfg.useSimulatedGateway = true;
    cfg.simulatedTickIntervalMs = 0;
    cfg.databasePath = dir.filePath(QStringLiteral("app.db"));

    Application app(cfg);
    // Composition root and visible widgets must share one state source.
    QCOMPARE(app.window()->shellModel(), app.shell());
#ifdef HLM_ENABLE_VISION
    QVERIFY(app.visionEnabled());
#else
    QVERIFY(!app.visionEnabled());
#endif
    app.start();
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app.gateway());
    QVERIFY(gw != nullptr);
    auto *db = app.database();
    QVERIFY(db != nullptr);
    auto *coord = app.coordinator();
    QVERIFY(coord != nullptr);
    auto *recipePage = app.window()->findChild<RecipeWidthPage *>();
    QVERIFY(recipePage != nullptr);

    // Wait for the DB worker to be ready (queued worker-thread path).
    QTRY_VERIFY_WITH_TIMEOUT(!db->isRestricted(), 5000);
    // A clean first launch must expose the administrator bootstrap without
    // requiring the user to discover it behind navigation/login controls.
    QTRY_COMPARE_WITH_TIMEOUT(app.window()->currentPageIndex(), 6, 5000);
    auto *usersPage = app.window()->findChild<UsersSettingsPage *>();
    QVERIFY(usersPage != nullptr);
    QCOMPARE(usersPage->currentPanel(), usersPage->createAdminPanel());

    // First run: create the initial admin, then log in as admin.
    QSignalSpy adminSpy(db, &DatabaseService::initialAdminCreated);
    QVERIFY(QMetaObject::invokeMethod(
        db, "createInitialAdmin", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
    QCOMPARE(adminSpy[0][0].toBool(), true);

    QSignalSpy loginSpy(db, &DatabaseService::loginResult);
    QVERIFY(QMetaObject::invokeMethod(
        db, "login", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.size() > 0, 5000);
    QVERIFY(loginSpy[0][0].value<LoginResult>().ok);
    // QSignalSpy records the worker-thread emission synchronously, while
    // Application::handleLoginResult is a queued sibling receiver that applies
    // the role on this thread; wait for the propagated end state.
    QTRY_COMPARE_WITH_TIMEOUT(coord->role(), Role::Admin, 5000);

    // D204 re-auth is single-flight and bound to the current session. A second
    // request cannot replace the value protected by the first verification.
    QSignalSpy passwordSpy(db, &DatabaseService::passwordVerified);
    emit usersPage->d204WriteRequested(5000, QStringLiteral("s3cret!"));
    emit usersPage->d204WriteRequested(6000, QStringLiteral("wrong"));
    QTRY_COMPARE_WITH_TIMEOUT(passwordSpy.count(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(gw->model().readRegister(kD204), quint16(5000), 5000);

    // Range validation is per field: 20000 is now inside the decoded D204
    // range, so the out-of-range value 40000 is used instead and must be
    // rejected before a password verification or PLC write is attempted
    // (PLC-HMI-005 D4).
    emit usersPage->d204WriteRequested(40000, QStringLiteral("s3cret!"));
    QCOMPARE(passwordSpy.count(), 1);
    QCOMPARE(gw->model().readRegister(kD204), quint16(5000));

    // Logout cancels an in-flight verification. Its successful result must not
    // write after the role has been downgraded.
    emit usersPage->d204WriteRequested(6000, QStringLiteral("s3cret!"));
    emit usersPage->logoutRequested();
    QTRY_COMPARE_WITH_TIMEOUT(passwordSpy.count(), 2, 5000);
    QCOMPARE(gw->model().readRegister(kD204), quint16(5000));
    QTRY_COMPARE_WITH_TIMEOUT(coord->role(), Role::Anonymous, 5000);

    loginSpy.clear();
    QVERIFY(QMetaObject::invokeMethod(
        db, "login", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.size() > 0, 5000);
    QVERIFY(loginSpy[0][0].value<LoginResult>().ok);
    QTRY_COMPARE_WITH_TIMEOUT(coord->role(), Role::Admin, 5000);

    // The administrator can add an operator through the real page -> database
    // wiring, and the confirmed result refreshes the visible list.
    QSignalSpy userAddedSpy(db, &DatabaseService::userAdded);
    emit usersPage->addUserRequested(QStringLiteral("operator"), Role::Operator,
                                     QStringLiteral("operator-pass"));
    QTRY_VERIFY_WITH_TIMEOUT(userAddedSpy.size() > 0, 5000);
    QCOMPARE(userAddedSpy[0][0].toBool(), true);
    QTRY_COMPARE_WITH_TIMEOUT(usersPage->userList()->count(), 2, 5000);

    // Home the machine via the raw gateway (M103 pulse + one M50=1 home-start
    // write + 2 s home return; PLC-HMI-011 D6: the pulse alone no longer
    // starts homing).
    gw->model().writeCoil(kM103, true);
    gw->model().writeCoil(kM103, false);
    gw->model().writeCoil(kM50, true);
    gw->tick();
    gw->tick();
    QVERIFY(gw->lastSnapshot().m9());

    // 回原点把 D130 清零 (SBR_HOME `DMOV K0 D130`): bring the width back to the
    // neutral target so the 300 run is the documented 100-count (7 s) run, and
    // so 自动准备完成 (M61 ∧ D128==D130 ∧ …) is restored (user decision
    // 2026-09-22).
    gw->model().writeRegister(kD204, 128); // an earlier case left D204 at 5000
    gw->model().writeCoil(43, true);
    gw->model().writeCoil(43, false);
    gw->tick();
    gw->tick(); // ceil(200 * 128 / 19200) = 2 s
    QCOMPARE(gw->lastSnapshot().currentWidth(), quint16(200));

    // Drive adjustWidth through the composition root: the recipe page emits
    // applyAdjustRequested, Application routes it to the coordinator, and the
    // M43 pulse must be submitted through the revised port (the wiring under
    // test). Converges on M44 + D130 == 300.
    QSignalSpy resultSpy(coord, &ControlCoordinator::commandResult);
    // D204 pinned so the composition-root run keeps the documented 7 s
    // duration: ceil(100 * 1280 / (15 * 1280)) = 7 s (PLC-HMI-005 D1/D2).
    gw->model().writeRegister(kD204, 1280);
    emit recipePage->applyAdjustRequested(300);
    QCOMPARE(gw->model().readRegister(kD128), quint16(300));
    gw->tick();
    QVERIFY(gw->lastSnapshot().m34()); // adjusting
    for (int i = 0; i < 7; ++i) // ceil(100 * 1280 / 19200) = 7 s
        gw->tick();
    QVERIFY(gw->lastSnapshot().m44());
    QCOMPARE(gw->lastSnapshot().currentWidth(), quint16(300));

    // The result must arrive via the coordinator -> recipe page path (not a
    // timeout). commandResult is emitted synchronously from the snapshot feed.
    QTRY_VERIFY_WITH_TIMEOUT(resultSpy.size() > 0, 5000);
    QCOMPARE(resultSpy[0][0].value<Command>(), Command::AdjustWidth);
    QCOMPARE(resultSpy[0][1].toBool(), true);
    QVERIFY(recipePage->statusText().contains(QStringLiteral("调宽完成")));

    app.shutdown();
}

QTEST_MAIN(FullFlowTest)
#include "test_full_flow.moc"
