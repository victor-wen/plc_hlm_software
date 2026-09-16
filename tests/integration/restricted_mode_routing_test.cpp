// PLC-HMI-008 black-box integration tests: restricted database mode is
// enforced by production command routing (brief OB-1..OB-4; contract ARCH-009
// and acceptance 736: "Restricted database mode production routing permits only
// Stop and software-estop set, independent of the current anonymous-role
// matrix").
//
// Restricted mode is entered through the production database path: the
// database storage is unusable at startup (database open/migration failure,
// architecture spec §13 "数据库迁移/写入失败 -> 受限模式"), which must put the
// composed application into restricted mode. While restricted:
//   * every non-safety production command request (reset, start, width adjust,
//     mode switch, manual actions, software-estop release) is rejected
//     immediately with exactly one visible, non-empty reason and never reaches
//     the PLC;
//   * the gate is identical for an administrator session and for an anonymous
//     session and does not depend on the permission matrix having downgraded
//     the user;
//   * online Stop and software-estop set still route to the PLC (OB-7);
//   * nothing is permanently latched: routing returns to the normal
//     permission/interlock behavior once storage is healthy again (spec §13:
//     repair storage, restart, migration succeeds).
//
// Authored only from .ai/test-briefs/PLC-HMI-008.yaml, the approved
// .ai/project-contract.yaml and inspectable test sources under tests/**. No
// production implementation source was read. Restricted-mode entry uses the
// real production database path, so no production symbol is invented; the
// optional flag probe below only reads an existing isRestricted() getter if one
// is present.

#include <QtTest>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "domain/operator_command_status.h"
#include "ports/iplc_gateway.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM100 = 100;
constexpr quint16 kM101 = 101;
constexpr quint16 kM102 = 102;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kM109 = 109;
constexpr quint16 kM110 = 110;
constexpr quint16 kD128 = 128;

// Simulator default target width of a fresh machine (also asserted by the
// existing operator-command tests).
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

// Reads an existing restricted-mode getter if the surface provides one. This
// never invents a production API: when no getter exists the probe reports
// "unavailable" and the behavioral assertions are the activation evidence.
template <typename T>
bool readRestrictedFlag(T *object, bool *value)
{
    if constexpr (requires { object->isRestricted(); }) {
        if (object == nullptr)
            return false;
        *value = object->isRestricted();
        return true;
    } else {
        Q_UNUSED(object);
        Q_UNUSED(value);
        return false;
    }
}

// Bounded wait for the application to settle after the database failure. When
// an existing isRestricted() getter is available, the wait ends as soon as it
// reports restricted mode.
void settleRestricted(Application &app, DatabaseService *db)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QTest::qWait(50);
        QApplication::processEvents();
        bool flag = false;
        const bool available =
            readRestrictedFlag(db, &flag) || readRestrictedFlag(&app, &flag);
        if (available && flag)
            return;
    }
}

// Extra activation evidence, asserted only when the existing getter is
// available on this tree.
void verifyRestrictedFlagIfExposed(Application &app, DatabaseService *db)
{
    bool flag = false;
    const bool available =
        readRestrictedFlag(db, &flag) || readRestrictedFlag(&app, &flag);
    if (available) {
        QVERIFY2(flag,
                 "an unusable database was expected to put the application into "
                 "restricted mode, but the restricted flag is false");
    }
}

void expectExactlyOneVisibleRejection(
    const QString &entry, const QVector<OperatorCommandStatus> &allStatuses,
    int statusMark, const QVector<quint16> &submittedAddresses,
    int submissionsBefore, std::optional<Command> expectedCommand)
{
    const QVector<OperatorCommandStatus> produced = allStatuses.mid(statusMark);
    QVERIFY2(!produced.isEmpty(),
             qPrintable(QStringLiteral("%1 produced no visible command state").arg(entry)));
    QCOMPARE(produced.size(), 1);
    const OperatorCommandStatus status = produced.first();
    QVERIFY2(status.lifecycle_state == OperatorCommandState::Rejected,
             qPrintable(QStringLiteral("%1 produced state %2 instead of a visible "
                                       "rejection")
                            .arg(entry, toString(status.lifecycle_state))));
    QVERIFY2(!status.human_readable_detail.trimmed().isEmpty(),
             qPrintable(QStringLiteral("%1 rejection carries no visible reason").arg(entry)));
    if (expectedCommand.has_value())
        QCOMPARE(status.command, *expectedCommand);
    QCOMPARE(submittedAddresses.size(), submissionsBefore);
}

} // namespace

class RestrictedModeRoutingTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1 / OB-2 / OB-3 ---------------------------------------------------
    void restrictedModeBlocksAdminCommandsWithoutPlcSubmission();
    void restrictedModeBlocksAnonymousCommandsIdentically();
    void restrictedModeRejectsRepeatedAttemptsWithoutLatchOrLaterCompletion();

    // --- OB-7: safety routing unchanged under restriction ---------------------
    void restrictedModeStillDispatchesStopAndSoftwareEstop();
    void restrictedModeStillRoutesStopAndSoftwareEstopForAnonymousSession();

    // --- OB-4: nothing permanently blocked once restriction is not active -----
    void healthyDatabaseKeepsNormalRoutingAvailable();
    void restrictedModeIsNotPermanentAfterStorageRecoveryAndRestart();
};

// --- OB-1 / OB-2 / OB-3 ---------------------------------------------------------

void RestrictedModeRoutingTest::restrictedModeBlocksAdminCommandsWithoutPlcSubmission()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY(started.app->shell() != nullptr);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");
    settleRestricted(*started.app, started.app->database());
    verifyRestrictedFlagIfExposed(*started.app, started.app->database());

    // Administrator session at the routing layer: permission alone would allow
    // these commands, so any rejection is the restricted-mode gate (OB-2).
    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(bar != nullptr);
    QVERIFY(recipe != nullptr);

    QVector<OperatorCommandStatus> statuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });
    QVector<quint16> submittedAddresses;
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&submittedAddresses](const SubmissionCompletion &c) {
                submittedAddresses.append(c.address);
            });

    const auto attempt = [&](const QString &entry, const std::function<void()> &submit,
                             std::optional<Command> command) {
        const int statusMark = int(statuses.size());
        const int submissionsBefore = int(submittedAddresses.size());
        submit();
        for (int i = 0; i < 6; ++i)
            started.gw->tick();
        QApplication::processEvents();
        expectExactlyOneVisibleRejection(entry, statuses, statusMark,
                                         submittedAddresses, submissionsBefore, command);
    };

    // Manual + homed: an unrestricted administrator would be allowed to adjust
    // width and operate the manual controls here.
    homeReady(*started.gw);

    attempt(QStringLiteral("adjustWidth"),
            [&] { emit recipe->applyAdjustRequested(300); }, Command::AdjustWidth);
    QVERIFY2(started.gw->model().readRegister(kD128) == kDefaultTargetWidth,
             "a blocked width adjust must not write D128");

    attempt(QStringLiteral("manualHold"),
            [&] { started.app->coordinator()->manualHold(kM106, true); }, std::nullopt);
    attempt(QStringLiteral("manualLatch"),
            [&] { started.app->coordinator()->manualLatch(kM109, true); }, std::nullopt);
    attempt(QStringLiteral("bypass"),
            [&] { started.app->coordinator()->bypass(kM110, true); }, std::nullopt);
    QVERIFY(!started.gw->model().readCoil(kM106));
    QVERIFY(!started.gw->model().readCoil(kM109));
    QVERIFY(!started.gw->model().readCoil(kM110));

    // Automatic + homed: an unrestricted administrator could start, reset and
    // switch mode here.
    putInAutoMode(*started.gw);
    QVERIFY(started.gw->lastSnapshot().m2());

    attempt(QStringLiteral("start"),
            [&] { emit bar->actionRequested(Command::Start); }, Command::Start);
    attempt(QStringLiteral("reset"),
            [&] { emit bar->actionRequested(Command::Reset); }, Command::Reset);
    attempt(QStringLiteral("modeSwitch"),
            [&] { emit bar->actionRequested(Command::ModeSwitch); }, Command::ModeSwitch);
    attempt(QStringLiteral("estopRelease"),
            [&] { emit bar->actionRequested(Command::EstopRelease); },
            Command::EstopRelease);

    QVERIFY(!started.gw->model().readCoil(kM101));
    QVERIFY(!started.gw->model().readCoil(kM103));
    QVERIFY2(started.gw->model().readCoil(kM104),
             "a blocked mode/reset command must not change the mode");

    started.shutdown();
}

void RestrictedModeRoutingTest::restrictedModeBlocksAnonymousCommandsIdentically()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");
    settleRestricted(*started.app, started.app->database());
    verifyRestrictedFlagIfExposed(*started.app, started.app->database());

    started.app->coordinator()->setRole(Role::Anonymous);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(bar != nullptr);
    QVERIFY(recipe != nullptr);

    QVector<OperatorCommandStatus> statuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });
    QVector<quint16> submittedAddresses;
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&submittedAddresses](const SubmissionCompletion &c) {
                submittedAddresses.append(c.address);
            });

    const auto attempt = [&](const QString &entry, const std::function<void()> &submit,
                             std::optional<Command> command) {
        const int statusMark = int(statuses.size());
        const int submissionsBefore = int(submittedAddresses.size());
        submit();
        for (int i = 0; i < 6; ++i)
            started.gw->tick();
        QApplication::processEvents();
        expectExactlyOneVisibleRejection(entry, statuses, statusMark,
                                         submittedAddresses, submissionsBefore, command);
    };

    homeReady(*started.gw);
    attempt(QStringLiteral("adjustWidth (anonymous)"),
            [&] { emit recipe->applyAdjustRequested(300); }, Command::AdjustWidth);
    attempt(QStringLiteral("manualHold (anonymous)"),
            [&] { started.app->coordinator()->manualHold(kM106, true); }, std::nullopt);
    attempt(QStringLiteral("bypass (anonymous)"),
            [&] { started.app->coordinator()->bypass(kM110, true); }, std::nullopt);

    putInAutoMode(*started.gw);
    attempt(QStringLiteral("start (anonymous)"),
            [&] { emit bar->actionRequested(Command::Start); }, Command::Start);
    attempt(QStringLiteral("reset (anonymous)"),
            [&] { emit bar->actionRequested(Command::Reset); }, Command::Reset);

    QVERIFY2(started.gw->model().readRegister(kD128) == kDefaultTargetWidth,
             "a blocked width adjust must not write D128");
    QVERIFY(!started.gw->model().readCoil(kM101));
    QVERIFY(!started.gw->model().readCoil(kM103));
    QVERIFY(!started.gw->model().readCoil(kM106));
    QVERIFY(!started.gw->model().readCoil(kM110));

    started.shutdown();
}

void RestrictedModeRoutingTest::restrictedModeRejectsRepeatedAttemptsWithoutLatchOrLaterCompletion()
{
    // OB-3 / contract invariant "every user request produces an immediate
    // visible ... rejected state": repeated blocked requests must each produce
    // their own fresh visible rejection, nothing may be silently swallowed or
    // latched by an earlier rejection, and no accepted/pending or later
    // terminal state may follow from a rejected request.
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");
    settleRestricted(*started.app, started.app->database());
    verifyRestrictedFlagIfExposed(*started.app, started.app->database());

    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(bar != nullptr);
    QVERIFY(recipe != nullptr);

    QVector<OperatorCommandStatus> statuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });
    QVector<quint16> submittedAddresses;
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&submittedAddresses](const SubmissionCompletion &c) {
                submittedAddresses.append(c.address);
            });

    homeReady(*started.gw);

    QVector<OperatorCommandStatus> rejected;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const int statusMark = int(statuses.size());
        const int submissionsBefore = int(submittedAddresses.size());
        emit recipe->applyAdjustRequested(300 + attempt);
        for (int i = 0; i < 6; ++i)
            started.gw->tick();
        QApplication::processEvents();
        expectExactlyOneVisibleRejection(
            QStringLiteral("repeated adjustWidth attempt %1").arg(attempt), statuses,
            statusMark, submittedAddresses, submissionsBefore, Command::AdjustWidth);
        rejected.append(statuses.constLast());
    }

    // No later accepted/pending or terminal state may follow a rejection, and
    // the recorded rejections must stay visible rejections with a reason.
    const int settledCount = int(statuses.size());
    for (int i = 0; i < 30; ++i)
        started.gw->tick();
    QApplication::processEvents();
    QCOMPARE(int(statuses.size()), settledCount);
    for (const OperatorCommandStatus &s : rejected) {
        QCOMPARE(s.lifecycle_state, OperatorCommandState::Rejected);
        QVERIFY2(!s.human_readable_detail.trimmed().isEmpty(),
                 "a repeated blocked request must carry a visible reason");
    }

    QVERIFY2(started.gw->model().readRegister(kD128) == kDefaultTargetWidth,
             "repeated blocked width adjusts must not write D128");

    started.shutdown();
}

// --- OB-7 -----------------------------------------------------------------------

void RestrictedModeRoutingTest::restrictedModeStillDispatchesStopAndSoftwareEstop()
{
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");
    settleRestricted(*started.app, started.app->database());
    verifyRestrictedFlagIfExposed(*started.app, started.app->database());

    started.app->coordinator()->setRole(Role::Admin);
    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    QVector<OperatorCommandStatus> statuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });

    homeReady(*started.gw);
    putInAutoMode(*started.gw);
    started.gw->model().writeCoil(kM101, true);
    started.gw->model().writeCoil(kM101, false);
    started.gw->tick();
    QVERIFY2(started.gw->lastSnapshot().m3(), "precondition: the machine must be running");

    const int stopMark = int(statuses.size());
    emit bar->actionRequested(Command::Stop);
    bool stopped = false;
    for (int i = 0; i < 20 && !stopped; ++i) {
        started.gw->tick();
        stopped = !started.gw->lastSnapshot().m3();
    }
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> stopStatuses = statuses.mid(stopMark);
    QVERIFY2(!stopStatuses.isEmpty(), "online stop produced no visible command state");
    for (const OperatorCommandStatus &s : stopStatuses) {
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "restricted mode must still route online stop");
    }
    QVERIFY2(stopped, "the routed stop must reach the PLC and clear the run state");

    const int estopMark = int(statuses.size());
    emit bar->actionRequested(Command::EstopSet);
    for (int i = 0; i < 4; ++i)
        started.gw->tick();
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> estopStatuses = statuses.mid(estopMark);
    QVERIFY2(!estopStatuses.isEmpty(), "software estop produced no visible command state");
    for (const OperatorCommandStatus &s : estopStatuses) {
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "restricted mode must still route software-estop set");
    }
    QVERIFY2(started.gw->model().readCoil(kM100),
             "the routed software estop must reach the PLC (M100)");

    started.shutdown();
}

void RestrictedModeRoutingTest::restrictedModeStillRoutesStopAndSoftwareEstopForAnonymousSession()
{
    // OB-1/OB-2/OB-7: the restricted-mode gate permits only Stop and
    // software-estop set independent of the current role, and the permission
    // matrix cannot be the explanation. The administrator variant is covered
    // above; this is the anonymous-session variant.
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");
    settleRestricted(*started.app, started.app->database());
    verifyRestrictedFlagIfExposed(*started.app, started.app->database());

    started.app->coordinator()->setRole(Role::Anonymous);
    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    QVector<OperatorCommandStatus> statuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });

    homeReady(*started.gw);
    putInAutoMode(*started.gw);
    started.gw->model().writeCoil(kM101, true);
    started.gw->model().writeCoil(kM101, false);
    started.gw->tick();
    QVERIFY2(started.gw->lastSnapshot().m3(), "precondition: the machine must be running");

    const int stopMark = int(statuses.size());
    emit bar->actionRequested(Command::Stop);
    bool stopped = false;
    for (int i = 0; i < 20 && !stopped; ++i) {
        started.gw->tick();
        stopped = !started.gw->lastSnapshot().m3();
    }
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> stopStatuses = statuses.mid(stopMark);
    QVERIFY2(!stopStatuses.isEmpty(),
             "restricted anonymous stop produced no visible command state");
    for (const OperatorCommandStatus &s : stopStatuses) {
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "restricted mode must still route online stop for an anonymous session");
    }
    QVERIFY2(stopped, "the routed stop must reach the PLC and clear the run state");

    const int estopMark = int(statuses.size());
    emit bar->actionRequested(Command::EstopSet);
    for (int i = 0; i < 4; ++i)
        started.gw->tick();
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> estopStatuses = statuses.mid(estopMark);
    QVERIFY2(!estopStatuses.isEmpty(),
             "restricted anonymous software estop produced no visible command state");
    for (const OperatorCommandStatus &s : estopStatuses) {
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "restricted mode must still route software-estop set for an "
                 "anonymous session");
    }
    QVERIFY2(started.gw->model().readCoil(kM100),
             "the routed software estop must reach the PLC (M100)");

    started.shutdown();
}

// --- OB-4 -----------------------------------------------------------------------

void RestrictedModeRoutingTest::healthyDatabaseKeepsNormalRoutingAvailable()
{
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    DatabaseService *db = started.app->database();
    QVERIFY(db != nullptr);
    QSignalSpy readySpy(db, &DatabaseService::recipesLoaded);
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);

    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY(started.gw->isOnline());
    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    QVector<OperatorCommandStatus> statuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });
    QVector<quint16> submittedAddresses;
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&submittedAddresses](const SubmissionCompletion &c) {
                submittedAddresses.append(c.address);
            });

    homeReady(*started.gw);
    const int mark = int(statuses.size());
    emit bar->actionRequested(Command::Reset);

    bool sawPulse = false;
    for (int i = 0; i < 40 && !sawPulse; ++i) {
        started.gw->tick();
        if (started.gw->model().readCoil(kM103) || submittedAddresses.contains(kM103))
            sawPulse = true;
    }
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = statuses.mid(mark);
    QVERIFY2(!produced.isEmpty(), "reset produced no visible command state");
    for (const OperatorCommandStatus &s : produced) {
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "a healthy database must not restrict production command routing");
    }
    QVERIFY2(sawPulse, "the accepted reset must reach the PLC (M103 pulse)");

    started.shutdown();
}

void RestrictedModeRoutingTest::restrictedModeIsNotPermanentAfterStorageRecoveryAndRestart()
{
    // Session 1: unusable storage -> restricted mode -> the administrator reset
    // is visibly rejected and never reaches the PLC.
    StartedApp started;
    started.makeStorageUnusable();
    started.start();
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");
    settleRestricted(*started.app, started.app->database());
    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *bar = started.app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    QVector<OperatorCommandStatus> statuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });
    QVector<quint16> submittedAddresses;
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&submittedAddresses](const SubmissionCompletion &c) {
                submittedAddresses.append(c.address);
            });

    homeReady(*started.gw);
    const int mark = int(statuses.size());
    const int submissionsBefore = int(submittedAddresses.size());
    emit bar->actionRequested(Command::Reset);
    for (int i = 0; i < 6; ++i)
        started.gw->tick();
    QApplication::processEvents();
    expectExactlyOneVisibleRejection(QStringLiteral("reset under restriction"), statuses,
                                     mark, submittedAddresses, submissionsBefore,
                                     Command::Reset);
    started.shutdown();

    // Storage repair per the architecture spec: fix the storage, restart the
    // application; the successful migration must end restricted mode.
    QVERIFY2(QFile::remove(started.cfg.databasePath),
             "the test could not remove the unusable database file");

    started.app.reset();
    started.gw = nullptr;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the restarted application must expose the gateway");
    DatabaseService *db = started.app->database();
    QVERIFY(db != nullptr);
    QSignalSpy readySpy(db, &DatabaseService::recipesLoaded);
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);
    started.advanceUntilOnline();
    QVERIFY(started.gw->isOnline());
    started.app->coordinator()->setRole(Role::Admin);

    ActionBar *restartedBar = started.app->window()->findChild<ActionBar *>();
    QVERIFY(restartedBar != nullptr);

    QVector<OperatorCommandStatus> restartedStatuses;
    connect(started.app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&restartedStatuses](const OperatorCommandStatus &s) {
                restartedStatuses.append(s);
            });
    QVector<quint16> restartedSubmissions;
    connect(started.gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&restartedSubmissions](const SubmissionCompletion &c) {
                restartedSubmissions.append(c.address);
            });

    homeReady(*started.gw);
    const int restartMark = int(restartedStatuses.size());
    emit restartedBar->actionRequested(Command::Reset);
    bool sawPulse = false;
    for (int i = 0; i < 40 && !sawPulse; ++i) {
        started.gw->tick();
        if (started.gw->model().readCoil(kM103) || restartedSubmissions.contains(kM103))
            sawPulse = true;
    }
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = restartedStatuses.mid(restartMark);
    QVERIFY2(!produced.isEmpty(),
             "reset after recovery produced no visible command state");
    for (const OperatorCommandStatus &s : produced) {
        QVERIFY2(s.lifecycle_state != OperatorCommandState::Rejected,
                 "the previously rejected reset must not stay permanently blocked "
                 "after storage recovery and restart");
    }
    QVERIFY2(sawPulse, "the routed reset after recovery must reach the PLC (M103 pulse)");

    started.shutdown();
}

QTEST_MAIN(RestrictedModeRoutingTest)
#include "restricted_mode_routing_test.moc"
