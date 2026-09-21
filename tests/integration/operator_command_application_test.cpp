// PLC-HMI-001 black-box integration tests: composition-root routing of
// operator-command feedback (brief OB-1, OB-5, OB-9, OB-10) into the persistent
// shell status surface.
//
// This file depends on the contract API introduced by the change
// (hlm::OperatorCommandStatus, ShellModel::operatorCommandStatus*,
// ActionBar::commandStatusLabel), so a compile failure of this target before
// implementation is the expected RED and is isolated here.

#include <QtTest>

#include <QApplication>
#include <QFile>
#include <QSpinBox>
#include <QTemporaryDir>

#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "app/lifecycle_controller.h"
#include "domain/operator_command_status.h"
#include "ports/iplc_gateway.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM103 = 103;
constexpr quint16 kM50 = 50; // PLC-HMI-011: home-start coil
constexpr quint16 kM104 = 104;
constexpr quint16 kM110 = 110;
constexpr quint16 kD130 = 130;

void homeReady(SimulatedPlcGateway &gw)
{
    gw.writeCoil(kM103, true);
    gw.writeCoil(kM103, false);
    gw.writeCoil(kM50, true); // PLC-HMI-011: homing starts on the home-start write
    gw.tick();
    gw.tick(); // home return takes 2 s
}

struct WarningCapture
{
    QVector<QtMsgType> types;
    QVector<QString> messages;
};

WarningCapture g_capture;
QtMessageHandler g_previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    g_capture.types.append(type);
    g_capture.messages.append(message);
}

bool capturedWarning()
{
    for (QtMsgType type : g_capture.types) {
        if (type == QtWarningMsg)
            return true;
    }
    return false;
}

// RAII handle that guarantees Application::shutdown() runs before the object is
// destroyed, so an early QVERIFY/QCOMPARE return from an (expected) failing
// assertion can never leave a live application to destruct uncleanly.
struct ApplicationShutdown
{
    void operator()(Application *app) const
    {
        if (app != nullptr)
            app->shutdown();
        delete app;
    }
};

using ApplicationHandle = std::unique_ptr<Application, ApplicationShutdown>;

// Builds a running application on the in-process simulator with an isolated
// database path. The simulated clock advances only through explicit ticks.
Application *startSimulatedApplication(const QTemporaryDir &dir, SimulatedPlcGateway **gwOut)
{
    auto *cfg = new AppConfig;
    cfg->useSimulatedGateway = true;
    cfg->simulatedTickIntervalMs = 0;
    cfg->databasePath = dir.filePath(QStringLiteral("app.db"));

    auto *app = new Application(*cfg);
    delete cfg;

    app->start();
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    if (gwOut != nullptr)
        *gwOut = gw;
    return app;
}

} // namespace

class OperatorCommandApplicationTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1: every entry point produces a visible state immediately --------
    void everyCommandEntryPointProducesVisibleState();

    // --- PLC-HMI-010 OB-1/OB-6: reset pending visible, then fixed-delay success
    void resetProjectsPendingThenFixedDelaySuccess();
    // --- PLC-HMI-010 OB-7: exactly one terminal per accepted reset -------------
    void resetTerminalIsEmittedExactlyOnce();
    // --- PLC-HMI-010 OB-3: restricted-mode reset stays visibly rejected --------
    void resetRejectedVisiblyInRestrictedModeWithoutSubmission();
    // --- PLC-HMI-010 OB-3/OB-7 (verify-phase edge case): repeated clicks -------
    void duplicateResetClicksWhilePendingAreRejectedAndOneTerminalSuccessProjected();

    // --- OB-9: coordinator terminal result is the projected adjust verdict ---
    void adjustTimeoutProjectsToShellAsTerminalFailure();

    // --- OB-10: unhandled command is an explicit visible diagnostic ----------
    void unhandledCommandProducesVisibleDiagnosticAndWarning();
};

// --- OB-1 ---------------------------------------------------------------------

void OperatorCommandApplicationTest::everyCommandEntryPointProducesVisibleState()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    // Bring the link up deterministically.
    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    ShellModel *shell = app->shell();
    ControlCoordinator *coord = app->coordinator();
    QVERIFY(shell != nullptr);
    QVERIFY(coord != nullptr);
    coord->setRole(Role::Anonymous);

    int statusChanges = 0;
    OperatorCommandStatus latest;
    connect(shell, &ShellModel::operatorCommandStatusChanged, this,
            [&](const OperatorCommandStatus &s) {
                ++statusChanges;
                latest = s;
            });

    const auto checkVisible = [&](const char *entry, int changesBefore) {
        QVERIFY2(statusChanges > changesBefore,
                 qPrintable(QStringLiteral("%1 produced no visible state")
                                .arg(QString::fromLatin1(entry))));
        const bool visibleRejection = latest.lifecycle_state
                == OperatorCommandState::Rejected
            && !latest.human_readable_detail.isEmpty();
        const bool visibleAcceptance = latest.lifecycle_state
                == OperatorCommandState::Accepted
            || latest.lifecycle_state == OperatorCommandState::Pending;
        QVERIFY2(visibleRejection || visibleAcceptance,
                 qPrintable(QStringLiteral("%1 produced state without detail: %2")
                                .arg(QString::fromLatin1(entry),
                                     latest.human_readable_detail)));
    };

    int changes = statusChanges;
    coord->reset();
    checkVisible("reset", changes);

    changes = statusChanges;
    coord->adjustWidth(300);
    checkVisible("adjustWidth", changes);

    changes = statusChanges;
    coord->setMode(true);
    checkVisible("setMode", changes);

    changes = statusChanges;
    coord->start();
    checkVisible("start", changes);

    changes = statusChanges;
    coord->estopRelease();
    checkVisible("estopRelease", changes);

    changes = statusChanges;
    coord->manualHold(106, true);
    checkVisible("manualHold press", changes);

    changes = statusChanges;
    coord->manualHold(106, false);
    checkVisible("manualHold release", changes);

    changes = statusChanges;
    coord->manualLatch(109, true);
    checkVisible("manualLatch set", changes);

    changes = statusChanges;
    coord->manualLatch(109, false);
    checkVisible("manualLatch clear", changes);

    changes = statusChanges;
    coord->bypass(kM110, true);
    checkVisible("bypass set", changes);

    changes = statusChanges;
    coord->bypass(kM110, false);
    checkVisible("bypass clear", changes);

    // Accepted/pending entry points available to an anonymous user.
    changes = statusChanges;
    coord->stop();
    checkVisible("stop", changes);

    changes = statusChanges;
    coord->estopSet();
    checkVisible("estopSet", changes);

    app->shutdown();
}

// --- PLC-HMI-010 OB-1 / OB-6 --------------------------------------------------

void OperatorCommandApplicationTest::resetProjectsPendingThenFixedDelaySuccess()
{
    // The reset is a fire-and-confirm-by-fixed-delay request: it must expose a
    // pending state with a non-empty human-readable detail that does NOT promise
    // a machine-confirmation step that no longer occurs, and then converge to a
    // single success terminal with the fixed detail 复位完成.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    ApplicationHandle app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    homeReady(*gw); // homed and left in manual mode: no mode switch is needed
    app->coordinator()->setRole(Role::Admin);

    // RAII: the connection is severed before the captured vector or the
    // application dies, so a failing assertion cannot crash the runner.
    QVector<OperatorCommandStatus> statuses;
    QObject statusScope;
    connect(app->shell(), &ShellModel::operatorCommandStatusChanged, &statusScope,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    emit bar->actionRequested(Command::Reset);

    // A pending state is visible before any terminal, with a non-empty detail.
    const OperatorCommandStatus first = app->shell()->operatorCommandStatus();
    QVERIFY2(!isTerminal(first.lifecycle_state) || statuses.size() > 1,
             "no pending reset state was projected before the terminal");
    bool sawPendingWithDetail = false;
    bool sawTerminal = false;
    bool pendingPromisesMachineConfirmation = false;
    for (const OperatorCommandStatus &s : statuses) {
        if (s.lifecycle_state == OperatorCommandState::Pending) {
            if (!s.human_readable_detail.trimmed().isEmpty())
                sawPendingWithDetail = true;
            // The superseded flow promised waiting for a machine-confirmation
            // step (M50/M61 homing confirmation); the fire-and-confirm reset
            // must not advertise it.
            if (s.human_readable_detail.contains(QStringLiteral("等待 PLC 确认"))
                || s.human_readable_detail.contains(QStringLiteral("等待确认"))
                || s.human_readable_detail.contains(QStringLiteral("回原点完成")))
                pendingPromisesMachineConfirmation = true;
        }
        if (isTerminal(s.lifecycle_state))
            sawTerminal = true;
    }

    // The fixed 200 ms completion is short: observe a bounded window of ticks.
    for (int i = 0; i < 20 && !sawTerminal; ++i) {
        gw->tick();
        const OperatorCommandStatus s = app->shell()->operatorCommandStatus();
        if (s.lifecycle_state == OperatorCommandState::Pending
            && !s.human_readable_detail.trimmed().isEmpty())
            sawPendingWithDetail = true;
        sawTerminal = isTerminal(s.lifecycle_state);
    }

    QVERIFY2(sawPendingWithDetail,
             "no pending reset status with a non-empty detail was projected");
    QVERIFY2(!pendingPromisesMachineConfirmation,
             "the reset pending detail still promises a machine-confirmation step");

    const OperatorCommandStatus latest = app->shell()->operatorCommandStatus();
    QVERIFY(isTerminal(latest.lifecycle_state));
    QVERIFY2(latest.lifecycle_state == OperatorCommandState::Succeeded,
             qPrintable(QStringLiteral("the reset terminal state was %1, not success")
                            .arg(toString(latest.lifecycle_state))));
    QCOMPARE(latest.human_readable_detail, QStringLiteral("复位完成"));

    app->shutdown();
}

void OperatorCommandApplicationTest::resetTerminalIsEmittedExactlyOnce()
{
    // OB-7: no accepted reset stays pending indefinitely and exactly one
    // terminal result is projected per accepted reset.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    ApplicationHandle app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    homeReady(*gw);
    app->coordinator()->setRole(Role::Admin);

    int terminalTransitions = 0;
    OperatorCommandStatus previous = app->shell()->operatorCommandStatus();
    QObject statusScope;
    connect(app->shell(), &ShellModel::operatorCommandStatusChanged, &statusScope,
            [&terminalTransitions, &previous](const OperatorCommandStatus &s) {
                if (s.command == Command::Reset
                    && isTerminal(s.lifecycle_state)
                    && !(previous.command == Command::Reset
                         && isTerminal(previous.lifecycle_state)))
                    ++terminalTransitions;
                if (s.command == Command::Reset)
                    previous = s;
            });

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    emit bar->actionRequested(Command::Reset);

    for (int i = 0; i < 30 && terminalTransitions == 0; ++i)
        gw->tick();

    QVERIFY2(terminalTransitions >= 1,
             "the accepted reset never converged to a terminal status");
    QCOMPARE(terminalTransitions, 1);

    // Further ticks must not produce a second terminal for the same reset.
    for (int i = 0; i < 20; ++i)
        gw->tick();
    QCOMPARE(terminalTransitions, 1);

    const OperatorCommandStatus latest = app->shell()->operatorCommandStatus();
    QVERIFY(isTerminal(latest.lifecycle_state));
    QCOMPARE(latest.human_readable_detail, QStringLiteral("复位完成"));

    app->shutdown();
}

// --- PLC-HMI-010 OB-3: restricted-mode reset -------------------------------

void OperatorCommandApplicationTest::resetRejectedVisiblyInRestrictedModeWithoutSubmission()
{
    // Restricted mode is entered through the production path (storage that
    // cannot hold a valid database, spec section 13). An administrator reset
    // must be rejected visibly with a non-empty reason and no reset signal may
    // be sent.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    AppConfig cfg;
    cfg.useSimulatedGateway = true;
    cfg.simulatedTickIntervalMs = 0;
    cfg.databasePath = dir.filePath(QStringLiteral("app.db"));

    QFile file(cfg.databasePath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("This file is not an SQLite database.");
    file.close();

    ApplicationHandle app(new Application(cfg));
    app->start();
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    QVERIFY(gw != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(app->lifecycle()->restricted(), 5000);

    for (int i = 0; i < 20 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());
    homeReady(*gw);
    gw->model().writeCoil(kM103, false); // observe only a new reset signal

    app->coordinator()->setRole(Role::Admin);

    QVector<OperatorCommandStatus> statuses;
    QObject statusScope;
    connect(app->shell(), &ShellModel::operatorCommandStatusChanged, &statusScope,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });
    int submissions = 0;
    QObject submissionScope;
    connect(gw, &SimulatedPlcGateway::submissionCompleted, &submissionScope,
            [&submissions](const SubmissionCompletion &) { ++submissions; });

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    emit bar->actionRequested(Command::Reset);
    for (int i = 0; i < 6; ++i)
        gw->tick();
    QApplication::processEvents();

    QVERIFY2(!statuses.isEmpty(), "the restricted reset produced no visible state");
    const OperatorCommandStatus status = statuses.last();
    QVERIFY2(status.lifecycle_state == OperatorCommandState::Rejected,
             qPrintable(QStringLiteral("the restricted reset state was %1, not a rejection")
                            .arg(toString(status.lifecycle_state))));
    QVERIFY2(!status.human_readable_detail.trimmed().isEmpty(),
             "the restricted reset rejection carried no reason");
    QCOMPARE(submissions, 0);
    QVERIFY2(!gw->model().readCoil(kM103),
             "the restricted reset sent the reset signal anyway");
}

// --- PLC-HMI-010 OB-3/OB-7 verify-phase edge case: repeated clicks ---------

void OperatorCommandApplicationTest::duplicateResetClicksWhilePendingAreRejectedAndOneTerminalSuccessProjected()
{
    // Repeated operator clicks on Reset while the first reset is pending must
    // each be projected as a visible rejection with a non-empty reason, and the
    // accepted reset must still project exactly one success terminal with the
    // fixed detail 复位完成 - never a second terminal, never a silent click.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    ApplicationHandle app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    homeReady(*gw); // homed, manual mode
    app->coordinator()->setRole(Role::Admin);

    QVector<OperatorCommandStatus> statuses;
    QObject statusScope;
    connect(app->shell(), &ShellModel::operatorCommandStatusChanged, &statusScope,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    // First click: accepted, then two further clicks while it is pending.
    emit bar->actionRequested(Command::Reset);
    QVERIFY(app->coordinator()->resetInProgress());
    emit bar->actionRequested(Command::Reset);
    emit bar->actionRequested(Command::Reset);

    int rejections = 0;
    for (const OperatorCommandStatus &s : statuses) {
        if (s.command == Command::Reset
            && s.lifecycle_state == OperatorCommandState::Rejected) {
            ++rejections;
            QVERIFY2(!s.human_readable_detail.trimmed().isEmpty(),
                     "a duplicate reset rejection carried no reason");
        }
    }
    QCOMPARE(rejections, 2);

    // Drive the fixed delay; the accepted reset must converge exactly once.
    bool sawTerminal = false;
    for (int i = 0; i < 30 && !sawTerminal; ++i) {
        gw->tick();
        sawTerminal = isTerminal(app->shell()->operatorCommandStatus().lifecycle_state);
    }
    QVERIFY2(sawTerminal, "the accepted reset never converged to a terminal status");

    // Further ticks must not add a second terminal or a second success.
    for (int i = 0; i < 20; ++i)
        gw->tick();

    int terminalEmissions = 0;
    int successEmissions = 0;
    for (const OperatorCommandStatus &s : statuses) {
        if (s.command != Command::Reset)
            continue;
        if (isTerminal(s.lifecycle_state))
            ++terminalEmissions;
        if (s.lifecycle_state == OperatorCommandState::Succeeded)
            ++successEmissions;
    }
    QCOMPARE(terminalEmissions, 1);
    QCOMPARE(successEmissions, 1);

    const OperatorCommandStatus latest = app->shell()->operatorCommandStatus();
    QVERIFY(isTerminal(latest.lifecycle_state));
    QCOMPARE(latest.human_readable_detail, QStringLiteral("复位完成"));

    app->shutdown();
}

// --- OB-9 ---------------------------------------------------------------------

void OperatorCommandApplicationTest::adjustTimeoutProjectsToShellAsTerminalFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());
    homeReady(*gw);
    app->coordinator()->setRole(Role::Admin);

    RecipeWidthPage *page = app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(page != nullptr);

    // Stall the motor so the coordinator's terminal result is a failure.
    gw->model().setPositioningStall(true);
    emit page->applyAdjustRequested(300);
    QVERIFY(gw->model().readRegister(128) == quint16(300)); // D128 written
    gw->tick();

    // PLC-HMI-005 parity supersession: the simulator now reports the stalled
    // width-adjust failure through the fixed 30 s (T6 K300) timeout instead of
    // the previous short dynamic window, so the injected tick clock must be
    // driven past the timeout before the shell can project a terminal state.
    // No assertion below is changed.
    for (int i = 0; i < 40; ++i)
        gw->tick();

    const OperatorCommandStatus terminal = app->shell()->operatorCommandStatus();
    QVERIFY(isTerminal(terminal.lifecycle_state));
    QVERIFY2(terminal.lifecycle_state == OperatorCommandState::TimedOut
                 || terminal.lifecycle_state == OperatorCommandState::Failed,
             qPrintable(QStringLiteral("adjust terminal state was %1")
                            .arg(toString(terminal.lifecycle_state))));
    QVERIFY(!terminal.human_readable_detail.isEmpty());

    // Later snapshots contain success-like bits, but the verdict must remain
    // the coordinator's terminal result (brief OB-9).
    gw->model().writeCoil(45, false);      // M45 off
    gw->model().writeCoil(34, false);      // M34 off
    gw->model().writeRegister(kD130, 300); // D130 == applied target
    gw->model().writeCoil(44, true);       // M44 on
    gw->tick();

    QVERIFY(app->shell()->operatorCommandStatus().lifecycle_state
            == terminal.lifecycle_state);
    QVERIFY2(!page->statusText().contains(QStringLiteral("调宽成功")),
             qPrintable(QStringLiteral("success-like snapshot overrode the "
                                       "coordinator verdict: %1")
                            .arg(page->statusText())));

    app->shutdown();
}

// --- OB-10 --------------------------------------------------------------------

void OperatorCommandApplicationTest::unhandledCommandProducesVisibleDiagnosticAndWarning()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    g_capture = {};
    g_previousHandler = qInstallMessageHandler(captureMessages);
    emit bar->actionRequested(static_cast<Command>(9999));
    qInstallMessageHandler(g_previousHandler);

    QVERIFY2(capturedWarning(), "unhandled command must emit a Qt warning");

    const OperatorCommandStatus status = app->shell()->operatorCommandStatus();
    QVERIFY2(status.lifecycle_state == OperatorCommandState::Rejected
                 || status.lifecycle_state == OperatorCommandState::Failed,
             qPrintable(QStringLiteral("unhandled command state was %1")
                            .arg(toString(status.lifecycle_state))));
    QVERIFY2(!status.human_readable_detail.isEmpty(),
             "unhandled command diagnostic must carry a human-readable detail");

    app->shutdown();
}

QTEST_MAIN(OperatorCommandApplicationTest)
#include "operator_command_application_test.moc"
