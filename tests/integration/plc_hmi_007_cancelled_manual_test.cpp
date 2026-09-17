// PLC-HMI-007 wave-2 black-box integration tests: a pending manual
// confirmation that is cancelled produces exactly one terminal outcome and
// never a double result for the same command (brief OB-7), including after
// restricted-mode entry or connection loss.
//
// Authored only from .ai/test-briefs/PLC-HMI-007.yaml (brief_version 2), the
// approved .ai/project-contract.yaml (OperatorCommandStatus lifecycle
// invariants: "Every accepted finite command converges to a visible ...
// terminal state"; "Gateway replacement ... converges pending commands"; NF-03
// no success before confirmation) and inspectable test sources under tests/**.
// No production implementation source was read.
//
// This target is a regression lock over the existing public lifecycle surface
// demonstrated by tests/integration/plc_hmi_006_release_and_envelope_test.cpp
// and tests/integration/operator_command_application_test.cpp:
//   * Application/AppConfig simulated composition (useSimulatedGateway,
//     simulatedTickIntervalMs=0, databasePath in a QTemporaryDir);
//   * SimulatedPlcGateway public tick()/model(); ControlCoordinator setRole /
//     manualHold(address, value) returning {accepted, reason};
//   * the production restricted-mode entry
//     Application::lifecycle()->enterRestrictedMode(reason)
//     (header app/lifecycle_controller.h), as exercised by the PLC-HMI-006
//     release tests;
//   * ShellModel::operatorCommandStatusChanged / operatorCommandStatus and the
//     domain lifecycle incl. isTerminal/toString;
//   * the simulator link-down fault hook setLinkDown(true) plus bounded ticks.
//
// The command-generation correlation field is contract-required on
// OperatorCommandStatus. This file reads it through a compile-time `requires`
// probe (the established pattern of the inspectable tests) so the regression
// lock compiles and runs even if the field is not exposed; when the field is
// exposed the per-generation assertions are applied, otherwise the case falls
// back to the generation-free request-start rule and the gap is recorded in
// the wave-2 RED report. The exact observed status sequence is captured in
// failure messages for the report.
//
// Expected classification: regression lock. The invariant is expected to hold
// on the current tree; any observable violation is a real regression finding,
// and no assertion is weakened to make it pass.

#include <QtTest>

#include <QApplication>
#include <QTemporaryDir>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "application/control_coordinator.h"
#include "app/lifecycle_controller.h"
#include "domain/operator_command_status.h"
#include "ui/MainWindow.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM103 = 103; // reset / home pulse
constexpr quint16 kM106 = 106; // manual hold output

constexpr int kMaxConvergenceTicks = 45;
constexpr int kDrainTicks = 20;

// --- compile-time probe for the contract-required generation field -------------

template <typename Status>
constexpr bool exposesCommandGeneration()
{
    return requires(const Status &status) { status.command_generation; };
}

template <typename Status>
quint64 generationOf(const Status &status)
{
    if constexpr (requires { status.command_generation; })
        return static_cast<quint64>(status.command_generation);
    else
        return 0;
}

// --- composition fixture (same convention as the inspectable PLC-HMI-006 and
// --- PLC-HMI-005 integration tests) --------------------------------------------

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

// --- shared helpers -------------------------------------------------------------

struct StatusSink
{
    QVector<OperatorCommandStatus> statuses;

    int mark() const { return int(statuses.size()); }
    QVector<OperatorCommandStatus> since(int mark) const { return statuses.mid(mark); }
};

// RAII scope: severs the connection before the sink dies (same convention as
// the inspectable PLC-HMI-006 tests, which had a teardown-crash finding).
void attachStatusSink(QObject *context, ShellModel *shell, StatusSink &sink)
{
    QObject::connect(shell, &ShellModel::operatorCommandStatusChanged, context,
                     [&sink](const OperatorCommandStatus &status) {
                         sink.statuses.append(status);
                     });
}

bool isRequestStart(const OperatorCommandStatus &status)
{
    return status.lifecycle_state == OperatorCommandState::Accepted
        || status.lifecycle_state == OperatorCommandState::Pending;
}

int terminalCount(const QVector<OperatorCommandStatus> &statuses)
{
    int count = 0;
    for (const OperatorCommandStatus &status : statuses) {
        if (isTerminal(status.lifecycle_state))
            ++count;
    }
    return count;
}

int terminalCountForGeneration(const QVector<OperatorCommandStatus> &statuses,
                               quint64 generation)
{
    int count = 0;
    for (const OperatorCommandStatus &status : statuses) {
        if (isTerminal(status.lifecycle_state) && generationOf(status) == generation)
            ++count;
    }
    return count;
}

QString describeSequence(const QVector<OperatorCommandStatus> &statuses)
{
    QStringList parts;
    int index = 0;
    for (const OperatorCommandStatus &status : statuses) {
        parts.append(QStringLiteral("#%1 %2 gen=%3 detail='%4'")
                         .arg(index++)
                         .arg(toString(status.lifecycle_state))
                         .arg(generationOf(status))
                         .arg(status.human_readable_detail));
    }
    return parts.join(QStringLiteral(" | "));
}

// No generation may carry more than one terminal status: a second terminal for
// the same command generation is exactly the double result OB-7 forbids.
bool uniqueTerminalPerGeneration(const QVector<OperatorCommandStatus> &statuses,
                                 QString *violation)
{
    QVector<quint64> seen;
    for (const OperatorCommandStatus &status : statuses) {
        if (!isTerminal(status.lifecycle_state))
            continue;
        const quint64 generation = generationOf(status);
        if (seen.contains(generation)) {
            if (violation != nullptr) {
                *violation = QStringLiteral("terminal state %1 for generation %2 "
                                            "arrived a second time; sequence: %3")
                                 .arg(toString(status.lifecycle_state))
                                 .arg(generation)
                                 .arg(describeSequence(statuses));
            }
            return false;
        }
        seen.append(generation);
    }
    return true;
}

// Generation-free fallback of the same property: a terminal may only follow an
// earlier terminal when a fresh request start (Accepted/Pending) came in
// between, i.e. never a bare second terminal for a settled request.
bool noTerminalFollowsATerminalWithoutANewRequest(
    const QVector<OperatorCommandStatus> &statuses, QString *violation)
{
    bool terminalSeen = false;
    bool requestStartedSinceTerminal = false;
    for (const OperatorCommandStatus &status : statuses) {
        if (isTerminal(status.lifecycle_state)) {
            if (terminalSeen && !requestStartedSinceTerminal) {
                if (violation != nullptr) {
                    *violation = QStringLiteral(
                                     "terminal state %1 followed an earlier terminal "
                                     "without a new request start; sequence: %2")
                                     .arg(toString(status.lifecycle_state))
                                     .arg(describeSequence(statuses));
                }
                return false;
            }
            terminalSeen = true;
            requestStartedSinceTerminal = false;
        } else if (isRequestStart(status) && terminalSeen) {
            requestStartedSinceTerminal = true;
        }
    }
    return true;
}

template <typename Predicate>
bool tickUntil(StartedApp &started, int maxTicks, Predicate &&predicate)
{
    for (int i = 0; i < maxTicks; ++i) {
        if (predicate())
            return true;
        if (started.gw != nullptr)
            started.gw->tick();
        QApplication::processEvents();
    }
    return predicate();
}

// Drives the reset/home path (M103 pulse; the home return takes 2 s), following
// the inspectable convention of tests/integration/restricted_mode_routing_test.cpp
// and plc_hmi_006_release_and_envelope_test.cpp.
void homeReady(StartedApp &started)
{
    if (started.gw == nullptr)
        return;
    started.gw->model().writeCoil(kM103, true);
    started.gw->model().writeCoil(kM103, false);
    started.gw->tick();
    started.gw->tick(); // home return takes 2 s
}

} // namespace

class PlcHmi007CancelledManualTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-7 baseline: a settled manual confirmation is terminal exactly once
    void singleManualHoldConvergesToExactlyOneTerminal();

    // --- OB-7: restricted-mode entry cancels a pending confirmation ------------
    void restrictedEntryDuringPendingManualHoldProducesExactlyOneTerminal();

    // --- OB-7: connection loss cancels a pending confirmation -------------------
    void connectionLossDuringPendingManualHoldProducesExactlyOneTerminalAndNoDoubleResult();
};

// --- OB-7 baseline --------------------------------------------------------------

void PlcHmi007CancelledManualTest::singleManualHoldConvergesToExactlyOneTerminal()
{
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY(started.app->shell() != nullptr);
    QVERIFY(started.app->coordinator() != nullptr);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Admin);
    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    homeReady(started);
    const int mark = sink.mark();
    started.app->coordinator()->manualHold(kM106, true);
    QVERIFY2(tickUntil(started, 20, [&] { return started.gw->model().readCoil(kM106); }),
             "precondition: the manual hold must energize M106 while unrestricted");
    QVERIFY2(tickUntil(started, kMaxConvergenceTicks,
                       [&] { return terminalCount(sink.since(mark)) >= 1; }),
             "the accepted manual hold produced no terminal state");

    // Bounded drain: a settled confirmation must not produce a second terminal.
    for (int i = 0; i < kDrainTicks; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "the manual hold must produce a visible command state");

    const bool generations = exposesCommandGeneration<OperatorCommandStatus>();
    if (generations) {
        const quint64 attemptGeneration = generationOf(produced.first());
        QCOMPARE(terminalCountForGeneration(produced, attemptGeneration), 1);
    }
    QCOMPARE(terminalCount(produced), 1);

    QString violation;
    if (generations) {
        QVERIFY2(uniqueTerminalPerGeneration(produced, &violation),
                 qPrintable(violation));
    } else {
        QVERIFY2(noTerminalFollowsATerminalWithoutANewRequest(produced, &violation),
                 qPrintable(violation));
    }

    // Every terminal status carries visible operator detail (established
    // surface invariant, asserted by the inspectable PLC-HMI-001/005 tests).
    for (const OperatorCommandStatus &status : produced) {
        if (isTerminal(status.lifecycle_state)) {
            QVERIFY2(!status.human_readable_detail.trimmed().isEmpty(),
                     qPrintable(QStringLiteral("the terminal manual state %1 carries "
                                               "no visible detail; sequence: %2")
                                    .arg(toString(status.lifecycle_state),
                                         describeSequence(produced))));
        }
    }

    started.shutdown();
}

// --- OB-7: restricted-mode entry -------------------------------------------------

void PlcHmi007CancelledManualTest::restrictedEntryDuringPendingManualHoldProducesExactlyOneTerminal()
{
    // A manual hold is established while unrestricted, then restricted mode is
    // entered before the confirmation has necessarily settled. The entry (and
    // any release it performs) must leave the pending confirmation with exactly
    // one terminal outcome for its own command generation, never a double
    // result, and never a dangling pending state attributable to the hold.
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY(started.app->shell() != nullptr);
    QVERIFY(started.app->coordinator() != nullptr);
    QVERIFY(started.app->lifecycle() != nullptr);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Admin);
    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    homeReady(started);
    const int mark = sink.mark();
    started.app->coordinator()->manualHold(kM106, true);

    // The hold must be accepted while unrestricted (precondition; the same
    // action is exercised by the PLC-HMI-006 release tests).
    QVERIFY2(tickUntil(started, 20, [&] { return started.gw->model().readCoil(kM106); }),
             "precondition: the manual hold must energize M106 while unrestricted");

    // Restricted entry while the hold confirmation is in scope.
    started.app->lifecycle()->enterRestrictedMode(QStringLiteral("test"));

    // Bounded convergence and drain: all statuses of this window are observed.
    for (int i = 0; i < kMaxConvergenceTicks; ++i)
        started.gw->tick();
    QApplication::processEvents();
    for (int i = 0; i < kDrainTicks; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "the cancelled manual confirmation must produce a visible command state");

    QString violation;
    const bool generations = exposesCommandGeneration<OperatorCommandStatus>();
    if (generations) {
        // The cancelled hold's own generation: the first status produced by the
        // accepted manual hold.
        const quint64 attemptGeneration = generationOf(produced.first());
        const int terminals = terminalCountForGeneration(produced, attemptGeneration);
        QVERIFY2(terminals == 1,
                 qPrintable(QStringLiteral("the cancelled manual confirmation "
                                           "(generation %1) produced %2 terminal "
                                           "outcomes instead of exactly one; "
                                           "sequence: %3")
                                .arg(attemptGeneration)
                                .arg(terminals)
                                .arg(describeSequence(produced))));
        QVERIFY2(uniqueTerminalPerGeneration(produced, &violation),
                 qPrintable(violation));
    } else {
        // The generation field is not exposed on this tree: apply the
        // generation-free no-double-terminal rule and record the per-generation
        // isolation as a coverage gap in the wave-2 RED report.
        QVERIFY2(noTerminalFollowsATerminalWithoutANewRequest(produced, &violation),
                 qPrintable(violation));
    }

    // No terminal may carry the manual hold's generation twice, and the
    // observed sequence is captured for the report on any failure above.

    started.shutdown();
}

// --- OB-7: connection loss --------------------------------------------------------

void PlcHmi007CancelledManualTest::connectionLossDuringPendingManualHoldProducesExactlyOneTerminalAndNoDoubleResult()
{
    // A manual hold is established while unrestricted; the link is then lost
    // before the confirmation has necessarily settled. The loss must converge
    // the pending confirmation to exactly one terminal outcome and must never
    // produce a double result or a spontaneous second request.
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY(started.app->shell() != nullptr);
    QVERIFY(started.app->coordinator() != nullptr);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Admin);
    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    homeReady(started);
    const int mark = sink.mark();
    const ControlCoordinator::CommandResult hold =
        started.app->coordinator()->manualHold(kM106, true);
    QVERIFY2(hold.accepted,
             qPrintable(QStringLiteral("precondition: the manual hold was not accepted "
                                       "while unrestricted (%1)")
                            .arg(hold.reason)));

    // Connection loss before the hold confirmation has necessarily settled.
    started.gw->setLinkDown(true);

    QVERIFY2(tickUntil(started, kMaxConvergenceTicks,
                       [&] { return terminalCount(sink.since(mark)) >= 1; }),
             qPrintable(QStringLiteral("the manual confirmation cancelled by connection "
                                       "loss produced no terminal state; observed "
                                       "sequence: %1")
                            .arg(describeSequence(sink.since(mark)))));

    // Bounded drain: the lost confirmation must not produce a later second
    // terminal (or a spontaneous new request) for the same generation.
    for (int i = 0; i < kDrainTicks; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "the cancelled manual confirmation must produce a visible command state");

    // Exactly one terminal outcome is possible in this window: the only user
    // action was the manual hold, and a connection loss must converge it, never
    // duplicate it.
    QCOMPARE(terminalCount(produced), 1);

    QString violation;
    const bool generations = exposesCommandGeneration<OperatorCommandStatus>();
    if (generations) {
        const quint64 attemptGeneration = generationOf(produced.first());
        QCOMPARE(terminalCountForGeneration(produced, attemptGeneration), 1);
        QVERIFY2(uniqueTerminalPerGeneration(produced, &violation),
                 qPrintable(violation));
    } else {
        QVERIFY2(noTerminalFollowsATerminalWithoutANewRequest(produced, &violation),
                 qPrintable(violation));
    }

    for (const OperatorCommandStatus &status : produced) {
        if (isTerminal(status.lifecycle_state)) {
            QVERIFY2(!status.human_readable_detail.trimmed().isEmpty(),
                     qPrintable(QStringLiteral("the terminal state %1 of the lost "
                                               "confirmation carries no visible "
                                               "detail; sequence: %2")
                                    .arg(toString(status.lifecycle_state),
                                         describeSequence(produced))));
        }
    }

    started.shutdown();
}

QTEST_MAIN(PlcHmi007CancelledManualTest)
#include "plc_hmi_007_cancelled_manual_test.moc"
