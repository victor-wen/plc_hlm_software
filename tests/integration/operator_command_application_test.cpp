// PLC-HMI-001 black-box integration tests: composition-root routing of
// operator-command feedback (brief OB-1, OB-5, OB-9, OB-10) into the persistent
// shell status surface.
//
// This file depends on the contract API introduced by the change
// (hlm::OperatorCommandStatus, ShellModel::operatorCommandStatus*,
// ActionBar::commandStatusLabel), so a compile failure of this target before
// implementation is the expected RED and is isolated here.

#include <QtTest>

#include <QSpinBox>
#include <QTemporaryDir>

#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "domain/operator_command_status.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM110 = 110;
constexpr quint16 kD130 = 130;

void homeReady(SimulatedPlcGateway &gw)
{
    gw.writeCoil(kM103, true);
    gw.writeCoil(kM103, false);
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

    // --- OB-5: reset pending detail names the manual switch and homing -------
    void resetPendingDetailNamesManualSwitchThenHoming();
    void resetFromManualHomedMachineConvergesVisibly();

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

// --- OB-5 ---------------------------------------------------------------------

void OperatorCommandApplicationTest::resetPendingDetailNamesManualSwitchThenHoming()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    // Homed machine in automatic mode: reset must first switch to manual mode.
    homeReady(*gw);
    gw->writeCoil(kM104, true);
    gw->tick();
    QVERIFY(gw->lastSnapshot().m2());

    app->coordinator()->setRole(Role::Admin);

    QVector<OperatorCommandStatus> statuses;
    connect(app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    emit bar->actionRequested(Command::Reset);

    const auto scan = [&statuses](bool &sawManual, bool &sawHoming,
                                  bool &sawTerminal) {
        for (const OperatorCommandStatus &s : statuses) {
            if (s.lifecycle_state == OperatorCommandState::Pending) {
                if (s.human_readable_detail.contains(QStringLiteral("手动")))
                    sawManual = true;
                if (s.human_readable_detail.contains(QStringLiteral("回原点"))
                    || s.human_readable_detail.contains(QStringLiteral("回零")))
                    sawHoming = true;
            }
            if (isTerminal(s.lifecycle_state))
                sawTerminal = true;
        }
    };

    bool sawManual = false;
    bool sawHoming = false;
    bool sawTerminal = false;
    scan(sawManual, sawHoming, sawTerminal);

    for (int i = 0; i < 6; ++i) {
        gw->tick();
        scan(sawManual, sawHoming, sawTerminal);
    }

    QVERIFY2(sawManual, "reset pending detail never mentioned the manual switch");
    QVERIFY2(sawHoming, "reset pending detail never mentioned homing");
    QVERIFY(sawTerminal);

    QVERIFY(!statuses.isEmpty());
    const OperatorCommandStatus latest = app->shell()->operatorCommandStatus();
    QVERIFY(isTerminal(latest.lifecycle_state));
    QVERIFY(!latest.human_readable_detail.isEmpty());

    app->shutdown();
}

void OperatorCommandApplicationTest::resetFromManualHomedMachineConvergesVisibly()
{
    // Brief boundary_cases: reset from manual mode and already-homed reset.
    // The machine is already homed and already in manual mode, so no mode
    // switch is needed, but the reset must still expose a homing pending
    // detail and converge to a visible terminal result.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    homeReady(*gw); // machine is homed and left in manual mode
    app->coordinator()->setRole(Role::Admin);

    QVector<OperatorCommandStatus> statuses;
    connect(app->shell(), &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);
    emit bar->actionRequested(Command::Reset);

    bool sawHoming = false;
    for (int i = 0; i < 20; ++i) {
        gw->tick();
        for (const OperatorCommandStatus &s : statuses) {
            if (s.lifecycle_state == OperatorCommandState::Pending
                && (s.human_readable_detail.contains(QStringLiteral("回原点"))
                    || s.human_readable_detail.contains(QStringLiteral("回零"))))
                sawHoming = true;
        }
        if (!statuses.isEmpty()
            && isTerminal(app->shell()->operatorCommandStatus().lifecycle_state))
            break;
    }

    QVERIFY2(sawHoming, "reset pending detail never mentioned homing");
    const OperatorCommandStatus latest = app->shell()->operatorCommandStatus();
    QVERIFY(isTerminal(latest.lifecycle_state));
    QVERIFY(!latest.human_readable_detail.isEmpty());

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

    for (int i = 0; i < 14; ++i)
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
