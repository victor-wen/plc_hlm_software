// PLC-HMI-008 developer regression tests (D5): restricted-mode production
// routing through the composed application.
//
// Covers the developer-owned seams behind the independent black-box tests:
// every production command entry (action bar, recipe apply, manual-control
// signals) is blocked while restricted with exactly one visible rejection
// carrying the deterministic LifecycleController reason and zero PLC
// submissions, identically for an administrator and an anonymous session;
// Stop and software-estop set still route; and the internal safety/clear paths
// (logoutClear) are never blocked by the restricted gate. Parameter/D204
// writes handled by Application are blocked visibly too, and a healthy
// database keeps normal routing.

#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QVector>

#include <functional>
#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "app/lifecycle_controller.h"
#include "application/control_coordinator.h"
#include "domain/operator_command_status.h"
#include "ports/iplc_gateway.h"
#include "ui/MainWindow.h"
#include "ui/pages/manual_control_page.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM42 = 42;
constexpr quint16 kM100 = 100;
constexpr quint16 kM101 = 101;
constexpr quint16 kM102 = 102;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kM109 = 109;
constexpr quint16 kM110 = 110;
constexpr quint16 kD122 = 122;
constexpr quint16 kD128 = 128;
constexpr quint16 kDefaultTargetWidth = 200;

void homeReady(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

void putInAutoMode(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM104, true);
    gw.tick();
}

struct StartedApp
{
    QTemporaryDir dir;
    AppConfig cfg;
    std::unique_ptr<Application> app;
    SimulatedPlcGateway *gw = nullptr;

    StartedApp()
    {
        cfg.useSimulatedGateway = true;
        cfg.simulatedTickIntervalMs = 0;
        cfg.databasePath = dir.filePath(QStringLiteral("app.db"));
    }

    // Production-path restriction trigger: storage that cannot hold a valid
    // database (the migration/open fails, spec §13).
    void makeStorageUnusable()
    {
        QFile file(cfg.databasePath);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 "the test could not create the unusable database file");
        file.write("This file is not an SQLite database.");
        file.close();
    }

    void start()
    {
        app = std::make_unique<Application>(cfg);
        app->start();
        gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
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
};

struct Recorder
{
    QVector<OperatorCommandStatus> statuses;
    QVector<quint16> submissions;
};

void expectRejected(const QString &entry, const Recorder &recorder, int statusMark,
                    int submissionMark, Command expected, const QString &reason)
{
    const QVector<OperatorCommandStatus> produced = recorder.statuses.mid(statusMark);
    QVERIFY2(!produced.isEmpty(),
             qPrintable(QStringLiteral("%1 produced no visible command state").arg(entry)));
    QCOMPARE(produced.size(), 1);
    const OperatorCommandStatus status = produced.first();
    QVERIFY2(status.lifecycle_state == OperatorCommandState::Rejected,
             qPrintable(QStringLiteral("%1 produced state %2 instead of a visible "
                                       "rejection")
                            .arg(entry, toString(status.lifecycle_state))));
    QCOMPARE(status.command, expected);
    QCOMPARE(status.human_readable_detail, reason);
    QCOMPARE(recorder.submissions.size(), submissionMark);
}

} // namespace

class RestrictedRoutingDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void restrictedAdminBlockedOnEveryProductionEntryWithoutSubmission();
    void restrictedAnonymousBlockedWithTheSameRestrictedReason();
    void restrictedModeStillRoutesStopAndSoftwareEstop();
    void restrictedModeKeepsLogoutClearWrites();
    void restrictedModeBlocksParameterWritesVisibly();
    void healthyDatabaseKeepsRoutingUnrestricted();
};

void RestrictedRoutingDeveloperTest::
    restrictedAdminBlockedOnEveryProductionEntryWithoutSubmission()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QTRY_VERIFY_WITH_TIMEOUT(started.app->lifecycle()->restricted(), 5000);
    QVERIFY(started.app->database()->isRestricted());
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    // D2: one deterministic, non-empty reason; Stop/EstopSet stay allowed.
    LifecycleController *lifecycle = started.app->lifecycle();
    const QString reason = lifecycle->commandRejectionReason();
    QVERIFY2(!reason.isEmpty(), "restricted mode must expose a non-empty reason");
    QVERIFY(reason.contains(QStringLiteral("受限")));
    QCOMPARE(lifecycle->commandRejectionReason(), reason);
    QVERIFY(lifecycle->commandAllowed(Command::Stop));
    QVERIFY(lifecycle->commandAllowed(Command::EstopSet));
    QVERIFY(!lifecycle->commandAllowed(Command::Reset));
    QVERIFY(!lifecycle->commandAllowed(Command::AdjustWidth));
    QVERIFY(!lifecycle->commandAllowed(Command::ManualCommand));

    // Administrator: the permission matrix alone would allow every command
    // below, so a rejection can only come from the restricted gate (D1/OB-2).
    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    ManualControlPage *manual = started.app->window()->findChild<ManualControlPage *>();
    QVERIFY(bar != nullptr);
    QVERIFY(recipe != nullptr);
    QVERIFY(manual != nullptr);

    Recorder recorder;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&recorder](const OperatorCommandStatus &s) {
                recorder.statuses.append(s);
            });
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&recorder](const SubmissionCompletion &c) {
                recorder.submissions.append(c.address);
            });

    const auto attempt = [&](const QString &entry, const std::function<void()> &submit,
                             Command expected) {
        const int statusMark = int(recorder.statuses.size());
        const int submissionMark = int(recorder.submissions.size());
        submit();
        for (int i = 0; i < 6; ++i)
            started.gw->tick();
        QApplication::processEvents();
        expectRejected(entry, recorder, statusMark, submissionMark, expected, reason);
    };

    // Ready machine: only the restricted gate can explain the rejections.
    homeReady(*started.gw);

    // ActionBar production entry (ActionBar -> MainWindow::commandRequested ->
    // Application::onCommandRequested).
    attempt(QStringLiteral("actionBar reset"),
            [&] { emit bar->actionRequested(Command::Reset); }, Command::Reset);
    attempt(QStringLiteral("actionBar start"),
            [&] { emit bar->actionRequested(Command::Start); }, Command::Start);
    attempt(QStringLiteral("actionBar modeSwitch"),
            [&] { emit bar->actionRequested(Command::ModeSwitch); },
            Command::ModeSwitch);
    attempt(QStringLiteral("actionBar estopRelease"),
            [&] { emit bar->actionRequested(Command::EstopRelease); },
            Command::EstopRelease);

    // Recipe page production entry (page signal -> coordinator adjustWidth).
    attempt(QStringLiteral("recipe apply"),
            [&] { emit recipe->applyAdjustRequested(300); }, Command::AdjustWidth);

    // Manual-control page production entries (page signal -> coordinator).
    attempt(QStringLiteral("manual hold"),
            [&] { emit manual->manualHoldRequested(kM106, true); },
            Command::ManualCommand);
    attempt(QStringLiteral("manual latch"),
            [&] { emit manual->manualLatchRequested(kM109, true); },
            Command::ManualCommand);
    attempt(QStringLiteral("manual bypass"),
            [&] { emit manual->bypassRequested(kM110, true); }, Command::Bypass);

    // No blocked command reached the PLC or changed machine state.
    QCOMPARE(started.gw->model().readRegister(kD128), kDefaultTargetWidth);
    QVERIFY(!started.gw->model().readCoil(kM101));
    QVERIFY(!started.gw->model().readCoil(kM103));
    QVERIFY(!started.gw->model().readCoil(kM106));
    QVERIFY(!started.gw->model().readCoil(kM109));
    QVERIFY(!started.gw->model().readCoil(kM110));

    started.shutdown();
}

void RestrictedRoutingDeveloperTest::restrictedAnonymousBlockedWithTheSameRestrictedReason()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QTRY_VERIFY_WITH_TIMEOUT(started.app->lifecycle()->restricted(), 5000);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Anonymous);
    const QString reason = started.app->lifecycle()->commandRejectionReason();
    QVERIFY(!reason.isEmpty());

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    ManualControlPage *manual = started.app->window()->findChild<ManualControlPage *>();
    QVERIFY(bar != nullptr);
    QVERIFY(recipe != nullptr);
    QVERIFY(manual != nullptr);

    Recorder recorder;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&recorder](const OperatorCommandStatus &s) {
                recorder.statuses.append(s);
            });
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&recorder](const SubmissionCompletion &c) {
                recorder.submissions.append(c.address);
            });

    const auto attempt = [&](const QString &entry, const std::function<void()> &submit,
                             Command expected) {
        const int statusMark = int(recorder.statuses.size());
        const int submissionMark = int(recorder.submissions.size());
        submit();
        for (int i = 0; i < 6; ++i)
            started.gw->tick();
        QApplication::processEvents();
        expectRejected(entry, recorder, statusMark, submissionMark, expected, reason);
    };

    homeReady(*started.gw);
    attempt(QStringLiteral("anonymous reset"),
            [&] { emit bar->actionRequested(Command::Reset); }, Command::Reset);
    attempt(QStringLiteral("anonymous recipe apply"),
            [&] { emit recipe->applyAdjustRequested(300); }, Command::AdjustWidth);
    attempt(QStringLiteral("anonymous manual hold"),
            [&] { emit manual->manualHoldRequested(kM106, true); },
            Command::ManualCommand);

    QVERIFY(!started.gw->model().readCoil(kM106));
    QCOMPARE(started.gw->model().readRegister(kD128), kDefaultTargetWidth);

    started.shutdown();
}

void RestrictedRoutingDeveloperTest::restrictedModeStillRoutesStopAndSoftwareEstop()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QTRY_VERIFY_WITH_TIMEOUT(started.app->lifecycle()->restricted(), 5000);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");
    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    Recorder recorder;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&recorder](const OperatorCommandStatus &s) {
                recorder.statuses.append(s);
            });
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&recorder](const SubmissionCompletion &c) {
                recorder.submissions.append(c.address);
            });

    homeReady(*started.gw);
    putInAutoMode(*started.gw);
    started.gw->model().writeCoil(kM101, true);
    started.gw->model().writeCoil(kM101, false);
    started.gw->tick();
    QVERIFY2(started.gw->lastSnapshot().m3(), "precondition: the machine must be running");

    const int stopMark = int(recorder.statuses.size());
    const int stopSubmissionMark = int(recorder.submissions.size());
    emit bar->actionRequested(Command::Stop);
    bool stopped = false;
    for (int i = 0; i < 20 && !stopped; ++i) {
        started.gw->tick();
        stopped = !started.gw->lastSnapshot().m3();
    }
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> stopStatuses =
        recorder.statuses.mid(stopMark);
    QVERIFY2(!stopStatuses.isEmpty(), "online stop produced no visible state");
    for (const OperatorCommandStatus &s : stopStatuses)
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "restricted mode must still route online stop");
    QVERIFY2(stopped, "the routed stop must reach the PLC");
    QVERIFY(recorder.submissions.mid(stopSubmissionMark).contains(kM102));

    const int estopMark = int(recorder.statuses.size());
    emit bar->actionRequested(Command::EstopSet);
    for (int i = 0; i < 4; ++i)
        started.gw->tick();
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> estopStatuses =
        recorder.statuses.mid(estopMark);
    QVERIFY2(!estopStatuses.isEmpty(), "software estop produced no visible state");
    for (const OperatorCommandStatus &s : estopStatuses)
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "restricted mode must still route software-estop set");
    QVERIFY2(started.gw->model().readCoil(kM100),
             "the routed software estop must reach the PLC (M100)");

    started.shutdown();
}

void RestrictedRoutingDeveloperTest::restrictedModeKeepsLogoutClearWrites()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QTRY_VERIFY_WITH_TIMEOUT(started.app->lifecycle()->restricted(), 5000);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    QSignalSpy clearedSpy(started.app->coordinator(),
                          &ControlCoordinator::continuousCleared);

    // Energize continuous outputs, then run the production logout-clear path:
    // restricted mode must never trap a de-energizing safety clear (D1).
    started.gw->model().writeCoil(kM42, true);
    for (quint16 a = kM106; a <= 111; ++a)
        started.gw->model().writeCoil(a, true);
    started.gw->tick();
    QVERIFY(started.gw->model().readCoil(kM42));
    QVERIFY(started.gw->model().readCoil(kM106));

    started.app->lifecycle()->onLogoutClearRequested();
    for (int i = 0; i < 10; ++i)
        started.gw->tick();
    QApplication::processEvents();

    QVERIFY2(!started.gw->model().readCoil(kM42),
             "logoutClear must keep clearing M42 under restriction");
    for (quint16 a = kM106; a <= 111; ++a)
        QVERIFY2(!started.gw->model().readCoil(a),
                 "logoutClear must keep clearing M106-M111 under restriction");
    QCOMPARE(clearedSpy.count(), 1);

    started.shutdown();
}

void RestrictedRoutingDeveloperTest::restrictedModeBlocksParameterWritesVisibly()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QTRY_VERIFY_WITH_TIMEOUT(started.app->lifecycle()->restricted(), 5000);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    UsersSettingsPage *users = started.app->window()->findChild<UsersSettingsPage *>();
    QVERIFY(users != nullptr);

    Recorder recorder;
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&recorder](const SubmissionCompletion &c) {
                recorder.submissions.append(c.address);
            });

    // Harness state only: restricted mode force-logs-out every user, so in
    // production the parameter panel is locked and a write cannot be
    // initiated. The shell admin state below only makes the page-local
    // projection observable; the restricted verdict stays lifecycle-driven
    // (the coordinator role is not changed).
    started.app->shell()->setUser(QStringLiteral("admin"), Role::Admin);
    QApplication::processEvents();

    const QString reason = started.app->lifecycle()->commandRejectionReason();
    const QString before = users->paramStatusText();
    emit users->writeParameterRequested(kD122, 100);
    for (int i = 0; i < 4; ++i)
        started.gw->tick();
    QApplication::processEvents();
    QVERIFY2(users->paramStatusText() != before,
             "a blocked parameter write must produce visible feedback");
    QVERIFY2(users->paramStatusText().contains(reason),
             qPrintable(QStringLiteral("parameter write feedback '%1' must carry the "
                                       "restricted reason")
                            .arg(users->paramStatusText())));
    QVERIFY(!recorder.submissions.contains(kD122));

    // D204 is gated before any password round-trip or register write: the
    // status keeps the restricted reason (not the password-verification text)
    // and no D128 submission is made.
    emit users->d204WriteRequested(1280, QStringLiteral("irrelevant-password"));
    QApplication::processEvents();
    QVERIFY2(users->paramStatusText().contains(reason),
             "the blocked D204 write must keep the restricted reason visible");
    QVERIFY2(!users->paramStatusText().contains(QStringLiteral("密码")),
             "the D204 password flow must not start while restricted");
    QVERIFY(!recorder.submissions.contains(kD128));

    started.shutdown();
}

void RestrictedRoutingDeveloperTest::healthyDatabaseKeepsRoutingUnrestricted()
{
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    DatabaseService *db = started.app->database();
    QVERIFY(db != nullptr);
    QSignalSpy readySpy(db, &DatabaseService::recipesLoaded);
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);

    QVERIFY(!started.app->lifecycle()->restricted());
    QVERIFY(started.app->lifecycle()->commandRejectionReason().isEmpty());
    started.advanceUntilOnline();
    QVERIFY(started.gw->isOnline());
    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    Recorder recorder;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&recorder](const OperatorCommandStatus &s) {
                recorder.statuses.append(s);
            });
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&recorder](const SubmissionCompletion &c) {
                recorder.submissions.append(c.address);
            });

    homeReady(*started.gw);
    const int mark = int(recorder.statuses.size());
    emit bar->actionRequested(Command::Reset);

    bool sawPulse = false;
    for (int i = 0; i < 40 && !sawPulse; ++i) {
        started.gw->tick();
        if (started.gw->model().readCoil(kM103)
            || recorder.submissions.contains(kM103))
            sawPulse = true;
    }
    QApplication::processEvents();

    QVERIFY2(!recorder.statuses.mid(mark).isEmpty(),
             "reset produced no visible command state");
    for (const OperatorCommandStatus &s : recorder.statuses.mid(mark))
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "a healthy database must not restrict production routing");
    QVERIFY2(sawPulse, "the accepted reset must reach the PLC (M103 pulse)");

    started.shutdown();
}

QTEST_MAIN(RestrictedRoutingDeveloperTest)
#include "restricted_routing_developer_test.moc"
