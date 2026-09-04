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
#include <QMouseEvent>
#include <QPushButton>
#include <QFile>
#include <QMetaObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "app/lifecycle_controller.h"
#include "application/control_coordinator.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/hold_button.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching AddressTable).
constexpr quint16 kM42 = 42;
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
    gw.writeCoil(kM103, true);
    gw.writeCoil(kM103, false);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

// Build a coordinator wired to the simulated gateway (same pattern as
// tests/unit/test_control_coordinator.cpp).
ControlCoordinator *makeCoordinator(SimulatedPlcGateway &gw, qint64 &now)
{
    ControlCoordinator::PulseTransport t;
    t.startPulse = [&gw](quint16 a) {
        gw.writeCoil(a, true);
        gw.writeCoil(a, false);
        return true;
    };
    t.writeHold = [&gw](quint16 a, bool v) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) {
        gw.writeCoil(a, v);
        return true;
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) {
        gw.writeRegister(a, v);
        return true;
    };
    auto *c = new ControlCoordinator(std::move(t), ControlCoordinator::Config(),
                                     [&now]() { return now; });
    QObject::connect(&gw, &SimulatedPlcGateway::snapshotReady, c,
                     [c](const DeviceSnapshot &s) { c->onSnapshot(s); });
    QObject::connect(&gw, &SimulatedPlcGateway::connectionStateChanged, c,
                     [c](bool online) { c->onConnectionChanged(online); });
    QObject::connect(&gw, &SimulatedPlcGateway::writeCompleted, c,
                     [c](quint16 a, bool ok, const QString &) { c->onWriteCompleted(a, ok); });
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

// --- 1. first-run DB + auth + session + role downgrade -----------------------

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

    // Drive the countdown to expiry deterministically.
    lc.setSessionTimeoutSec(3);
    lc.startSessionTimer();
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

    // 复位 (M103 pulse) -> home return completes.
    QVERIFY(c->reset().accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m50()); // homing
    gw.tick();
    QVERIFY(gw.lastSnapshot().m9()); // M61 via M9: homed
    QVERIFY(!gw.lastSnapshot().m50());
    QVERIFY(accepted.contains(Command::Reset));
    QVERIFY(results.contains({Command::Reset, true}));

    // 配方调宽 300: D128 written, M43 pulse, converges on M44 + D130=300.
    QVERIFY(c->adjustWidth(300).accepted);
    QCOMPARE(gw.model().readRegister(kD128), quint16(300));
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34()); // adjusting
    for (int i = 0; i < 7; ++i) // ceil(100/15) = 7 s
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

    // Not homed: adjustWidth rejected by the interlock gate.
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

    // Stall the motor: positioning never completes -> dynamic timeout.
    gw.model().setPositioningStall(true);
    QVERIFY(c->adjustWidth(300).accepted);
    gw.tick();
    QVERIFY(gw.lastSnapshot().m34());

    // Timeout = ceil(100/15) + 5 = 12 s (spec §10.3.1).
    for (int i = 0; i < 12; ++i)
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

    // Reset clears the latched fault.
    QVERIFY(c->reset().accepted);
    gw.tick();
    gw.tick();
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
    QSignalSpy writeSpy(&gw, &SimulatedPlcGateway::writeCompleted);

    gw.setLinkDown(true);
    QVERIFY(!gw.isOnline());
    QVERIFY(!c->online());
    QCOMPARE(connSpy.size(), 1);
    QCOMPARE(connSpy[0][0].toBool(), false);

    // Writes rejected while offline, not applied.
    gw.writeCoil(kM100, true);
    QCOMPARE(writeSpy.size(), 1);
    QVERIFY(!writeSpy[0][1].toBool());
    QVERIFY(!gw.model().readCoil(kM100));

    // Restore: still offline until the next tick reconnects.
    gw.setLinkDown(false);
    QVERIFY(!gw.isOnline());
    gw.tick();
    QVERIFY(gw.isOnline());
    QVERIFY(c->online());
    QCOMPARE(connSpy.size(), 2);
    QCOMPARE(connSpy[1][0].toBool(), true);

    // Writes accepted again.
    gw.writeCoil(kM100, true);
    QCOMPARE(writeSpy.size(), 2);
    QVERIFY(writeSpy[1][0].toBool());
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
        r.targetWidthMm = 300;
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
    QCOMPARE(recipes[0].targetWidthMm, 300);
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

    // Anonymous role: action buttons disabled with a tooltip reason
    // (spec §11.4: 无权限操作保持可发现但禁用并说明原因).
    QVERIFY(!w.startButton()->isEnabled());
    QVERIFY(!w.startButton()->toolTip().isEmpty());
    QVERIFY(!w.resetButton()->isEnabled());
    QVERIFY(!w.resetButton()->toolTip().isEmpty());
    QVERIFY(!w.estopButton()->isEnabled());
    QVERIFY(!w.estopButton()->toolTip().isEmpty());
}

// --- 7. composition root (Application) adjustWidth convergence ----------------
//
// Regression test for the Task 20 review finding: Application::wireGateway
// must connect writeCompleted to ControlCoordinator::onWriteCompleted, or the
// M43 pulse is never sent and every adjustWidth times out with
// "调宽等待超时". This test drives the real composition root (Application),
// not a hand-rolled coordinator, so the wiring bug is exercised.

void FullFlowTest::applicationAdjustWidthConverges()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    AppConfig cfg;
    cfg.useSimulatedGateway = true;
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
    QCOMPARE(coord->role(), Role::Admin);

    // Home the machine via the raw gateway (M103 pulse + 2 s home return).
    gw->writeCoil(kM103, true);
    gw->writeCoil(kM103, false);
    gw->tick();
    gw->tick();
    QVERIFY(gw->lastSnapshot().m9());

    // Drive adjustWidth through the composition root: the recipe page emits
    // applyAdjustRequested, Application routes it to the coordinator, and the
    // coordinator's M43 pulse must be fed by writeCompleted (the wiring under
    // test). Converges on M44 + D130 == 300.
    QSignalSpy resultSpy(coord, &ControlCoordinator::commandResult);
    emit recipePage->applyAdjustRequested(300);
    QCOMPARE(gw->model().readRegister(kD128), quint16(300));
    gw->tick();
    QVERIFY(gw->lastSnapshot().m34()); // adjusting
    for (int i = 0; i < 7; ++i) // ceil(100/15) = 7 s
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
