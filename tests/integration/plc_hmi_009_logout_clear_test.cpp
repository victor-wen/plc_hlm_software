// PLC-HMI-009 OB-1 black-box integration tests (independent): honest
// logout/session-timeout clear convergence (M42, M106-M111 off).
//
// Authored only from .ai/test-briefs/PLC-HMI-009.yaml (brief_version 1), the
// approved .ai/project-contract.yaml and inspectable test sources under
// tests/**. No production implementation source was read.
//
// Brief OB-1 observable assertions covered here:
//   * a clear whose writes are accepted but never confirmed produces a visible
//     request-start (accepted/pending) status and never a success status;
//   * a confirmed clear (all seven bits observable as 0) reports exactly one
//     success terminal with a non-empty detail and never duplicates it;
//   * a rejected write (offline/transport rejection) produces a visible failure
//     terminal immediately, with no success and no indefinite pending state;
//   * application shutdown with an unconfirmed clear emits no success status
//     (a shutdown that hangs indefinitely is caught by the harness timeout).
//
// Surface used (all pre-existing in inspectable test sources):
//   * Application/AppConfig simulated composition (useSimulatedGateway,
//     simulatedTickIntervalMs=0, databasePath in a QTemporaryDir) - the
//     PLC-HMI-006/007 integration convention;
//   * SimulatedPlcGateway public tick()/isOnline()/setLinkDown(true)/model()
//     with readCoil(address);
//   * LifecycleController::onLogoutClearRequested() (header
//     app/lifecycle_controller.h), reached through Application::lifecycle();
//   * ShellModel::operatorCommandStatusChanged and the domain
//     OperatorCommandStatus lifecycle incl. isTerminal/toString.
//
// Expected RED on the current tree: the clear reports success as soon as the
// write requests are accepted by the transport, so the never-confirmed cases
// (neverConfirmedClearIsNotReportedAsCleared and
// shutdownWithPendingClearClaimsNoSuccess) observe a fabricated success until
// OB-1 is implemented.

#include <QtTest>

#include <QApplication>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "app/lifecycle_controller.h"
#include "domain/operator_command_status.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable and the
// inspectable PLC-HMI-006/007 integration tests).
constexpr int kM42 = 42;
constexpr int kFirstManualOutput = 106;
constexpr int kLastManualOutput = 111;

// PLC-HMI-011 OB-9: the HMI-writable home-start coil. The online logout /
// session-timeout clear must release it together with the existing continuous
// outputs (M42, M106-M111) without touching M100 (estop) or M105.
constexpr int kM50 = 50;
constexpr int kM100 = 100;
constexpr int kM105 = 105;

constexpr int kConfirmedClearTicks = 30;
constexpr int kOfflineSettleTicks = 20;
constexpr int kOfflineClearTicks = 10;

// --- composition fixture (established integration convention) ------------------

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

// RAII scope: severs the connection before the sink dies (the inspectable
// PLC-HMI-006 tear-down finding: never leave a lambda connected to a dead sink).
void attachStatusSink(QObject *context, ShellModel *shell, StatusSink &sink)
{
    QObject::connect(shell, &ShellModel::operatorCommandStatusChanged, context,
                     [&sink](const OperatorCommandStatus &status) {
                         sink.statuses.append(status);
                     });
}

// Terminal = Failed / TimedOut / CommunicationsLost / Succeeded / Rejected /
// GatewayReplaced; isTerminal() exists in domain/operator_command_status.h.
bool isSuccessOf(const OperatorCommandStatus &status)
{
    return status.lifecycle_state == OperatorCommandState::Succeeded
        && status.human_readable_detail.contains(QStringLiteral("连续输出已清除"));
}

bool isRequestStart(const OperatorCommandStatus &status)
{
    return status.lifecycle_state == OperatorCommandState::Accepted
        || status.lifecycle_state == OperatorCommandState::Pending;
}

bool isLogoutClear(const OperatorCommandStatus &status)
{
    return status.command == Command::LogoutClear;
}

// A success terminal for the logout clear itself, independent of exact wording
// (the brief fixes "a non-empty detail", not the message text).
bool isClearSuccess(const OperatorCommandStatus &status)
{
    return isLogoutClear(status)
        && status.lifecycle_state == OperatorCommandState::Succeeded;
}

QString describeSequence(const QVector<OperatorCommandStatus> &statuses)
{
    QStringList parts;
    int index = 0;
    for (const OperatorCommandStatus &status : statuses) {
        parts.append(QStringLiteral("#%1 cmd=%2 %3 detail='%4'")
                         .arg(index++)
                         .arg(int(status.command))
                         .arg(toString(status.lifecycle_state))
                         .arg(status.human_readable_detail));
    }
    return parts.join(QStringLiteral(" | "));
}

} // namespace

class PlcHmi009LogoutClearTest : public QObject
{
    Q_OBJECT

private slots:
    void neverConfirmedClearIsNotReportedAsCleared();
    void confirmedClearReportsSuccessExactlyOnce();
    void offlineClearConvergesToVisibleFailure();
    void shutdownWithPendingClearClaimsNoSuccess();
    void duplicateClearWhilePendingStartsNoSecondLifecycle();
    void clearLeavesM100AndM105Untouched();
    // --- PLC-HMI-011 OB-9: the clear also releases the home-start bit ----------
    void clearReleasesTheHomeStartBitWithTheContinuousOutputs();
    void cleanupTestCase() {}
};

// --- OB-1: an unconfirmed clear must never be reported as cleared ---------------

void PlcHmi009LogoutClearTest::neverConfirmedClearIsNotReportedAsCleared()
{
    // Brief OB-1: without a peer confirmation the clear is not cleared. The
    // write requests may be accepted by the transport, so a request-start must
    // be visible, but no success may appear.
    StartedApp started;
    started.start();
    QVERIFY2(started.app != nullptr, "the application must be composed");
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY2(started.app->shell() != nullptr, "the composed application must expose the shell");
    QVERIFY2(started.app->lifecycle() != nullptr,
             "the composed application must expose the lifecycle controller");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    started.app->lifecycle()->onLogoutClearRequested();
    QApplication::processEvents(); // no gateway tick: no transfer can complete

    const QVector<OperatorCommandStatus> produced = sink.statuses;

    // (1) Honesty: no success status may exist without peer confirmation.
    int fabricatedSuccesses = 0;
    QString fabricatedDetails;
    for (const OperatorCommandStatus &status : produced) {
        if (isSuccessOf(status)) {
            ++fabricatedSuccesses;
            fabricatedDetails += QStringLiteral("[%1]").arg(status.human_readable_detail);
        }
    }
    QVERIFY2(fabricatedSuccesses == 0,
             qPrintable(QStringLiteral("the never-confirmed logout clear reported %1 "
                                       "success status(es) %2 although no transfer was "
                                       "allowed to complete; observed sequence: %3")
                            .arg(fabricatedSuccesses)
                            .arg(fabricatedDetails)
                            .arg(describeSequence(produced))));

    // (2) Strictly stronger: no Succeeded state for the clear under any wording.
    int succeededStates = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (isClearSuccess(status))
            ++succeededStates;
    }
    QVERIFY2(succeededStates == 0,
             qPrintable(QStringLiteral("the never-confirmed logout clear produced %1 "
                                       "Succeeded state(s); observed sequence: %2")
                            .arg(succeededStates)
                            .arg(describeSequence(produced))));

    // (3) Non-vacuity and OB-1: the request must be visibly started.
    int requestStarts = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (isLogoutClear(status) && isRequestStart(status))
            ++requestStarts;
    }
    QVERIFY2(requestStarts >= 1,
             qPrintable(QStringLiteral("the logout clear produced no visible request-start "
                                       "(accepted/pending) status; observed sequence: %1")
                            .arg(describeSequence(produced))));

    started.shutdown();
}

// --- OB-1: a confirmed clear reports success exactly once ------------------------

void PlcHmi009LogoutClearTest::confirmedClearReportsSuccessExactlyOnce()
{
    // Brief OB-1: once the peer confirms the clear (all seven bits observable
    // as 0), exactly one success terminal appears with a non-empty detail and
    // the outputs stay cleared.
    StartedApp started;
    started.start();
    QVERIFY2(started.app != nullptr, "the application must be composed");
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY2(started.app->shell() != nullptr, "the composed application must expose the shell");
    QVERIFY2(started.app->lifecycle() != nullptr,
             "the composed application must expose the lifecycle controller");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    started.app->lifecycle()->onLogoutClearRequested();
    for (int i = 0; i < kConfirmedClearTicks; ++i) {
        started.gw->tick();
        QApplication::processEvents();
    }

    const QVector<OperatorCommandStatus> produced = sink.statuses;

    int successCount = 0;
    bool everySuccessHasDetail = true;
    QString successDetails;
    for (const OperatorCommandStatus &status : produced) {
        if (!isClearSuccess(status))
            continue;
        ++successCount;
        if (status.human_readable_detail.trimmed().isEmpty())
            everySuccessHasDetail = false;
        successDetails += QStringLiteral("[%1]").arg(status.human_readable_detail);
    }
    QVERIFY2(successCount == 1,
             qPrintable(QStringLiteral("the confirmed logout clear reported %1 success "
                                       "terminal(s) %2 instead of exactly one; observed "
                                       "sequence: %3")
                            .arg(successCount)
                            .arg(successDetails)
                            .arg(describeSequence(produced))));
    QVERIFY2(everySuccessHasDetail,
             qPrintable(QStringLiteral("the confirmed clear success carries no visible "
                                       "detail; observed sequence: %1")
                            .arg(describeSequence(produced))));

    // Convergence: the clear settles terminally (no dangling pending state).
    QVERIFY2(isTerminal(produced.last().lifecycle_state),
             qPrintable(QStringLiteral("the confirmed clear did not converge to a terminal "
                                       "state; observed sequence: %1")
                            .arg(describeSequence(produced))));

    // The peer-confirmed bits are off in the observable machine state.
    QVERIFY2(!started.gw->model().readCoil(kM42),
             "the confirmed clear must leave M42 off");
    for (int address = kFirstManualOutput; address <= kLastManualOutput; ++address) {
        QVERIFY2(!started.gw->model().readCoil(address),
                 qPrintable(QStringLiteral("the confirmed clear must leave M%1 off")
                                .arg(address)));
    }

    started.shutdown();
}

// --- OB-1: an offline/rejected clear converges to a visible failure -------------

void PlcHmi009LogoutClearTest::offlineClearConvergesToVisibleFailure()
{
    // Brief OB-1 boundary: with the link down every write is rejected, so the
    // clear must produce a visible failure terminal immediately, must never
    // report success, and must never remain pending indefinitely.
    StartedApp started;
    started.start();
    QVERIFY2(started.app != nullptr, "the application must be composed");
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY2(started.app->shell() != nullptr, "the composed application must expose the shell");
    QVERIFY2(started.app->lifecycle() != nullptr,
             "the composed application must expose the lifecycle controller");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    // Public fault hook: drive the link down and let the gateway transition.
    started.gw->setLinkDown(true);
    for (int i = 0; i < kOfflineSettleTicks && started.gw->isOnline(); ++i)
        started.gw->tick();
    QVERIFY2(!started.gw->isOnline(),
             "precondition: the link-down fault must drive the gateway offline");

    const int mark = sink.mark();
    started.app->lifecycle()->onLogoutClearRequested();
    for (int i = 0; i < kOfflineClearTicks; ++i) {
        started.gw->tick();
        QApplication::processEvents();
    }

    const QVector<OperatorCommandStatus> produced = sink.since(mark);

    int fabricatedSuccesses = 0;
    QString fabricatedDetails;
    for (const OperatorCommandStatus &status : produced) {
        if (isSuccessOf(status)) {
            ++fabricatedSuccesses;
            fabricatedDetails += QStringLiteral("[%1]").arg(status.human_readable_detail);
        }
    }
    QVERIFY2(fabricatedSuccesses == 0,
             qPrintable(QStringLiteral("the offline logout clear reported %1 success "
                                       "status(es) %2; observed sequence: %3")
                            .arg(fabricatedSuccesses)
                            .arg(fabricatedDetails)
                            .arg(describeSequence(produced))));

    int succeededStates = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (isClearSuccess(status))
            ++succeededStates;
    }
    QVERIFY2(succeededStates == 0,
             qPrintable(QStringLiteral("the offline logout clear produced %1 Succeeded "
                                       "state(s); observed sequence: %2")
                            .arg(succeededStates)
                            .arg(describeSequence(produced))));

    bool failureTerminalWithDetail = false;
    const OperatorCommandStatus *lastClearStatus = nullptr;
    int clearStatuses = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (!isLogoutClear(status))
            continue;
        ++clearStatuses;
        lastClearStatus = &status;
        if (isTerminal(status.lifecycle_state)
            && status.lifecycle_state != OperatorCommandState::Succeeded
            && !status.human_readable_detail.trimmed().isEmpty()) {
            failureTerminalWithDetail = true;
        }
    }
    QVERIFY2(clearStatuses >= 1,
             qPrintable(QStringLiteral("the offline logout clear produced no visible "
                                       "command state; observed sequence: %1")
                            .arg(describeSequence(produced))));
    QVERIFY2(failureTerminalWithDetail,
             qPrintable(QStringLiteral("the offline logout clear produced no visible "
                                       "non-success terminal with a non-empty detail; "
                                       "observed sequence: %1")
                            .arg(describeSequence(produced))));
    QVERIFY2(lastClearStatus != nullptr && isTerminal(lastClearStatus->lifecycle_state),
             qPrintable(QStringLiteral("the offline logout clear did not converge to a "
                                       "terminal state; observed sequence: %1")
                            .arg(describeSequence(produced))));

    started.shutdown();
}

// --- OB-1: shutdown with a pending clear must not claim success ------------------

void PlcHmi009LogoutClearTest::shutdownWithPendingClearClaimsNoSuccess()
{
    // Brief OB-1: application shutdown with an unconfirmed clear emits no
    // success status for the clear. The shutdown must also not hang
    // indefinitely; an unbounded hang is caught by the harness timeout.
    StartedApp started;
    started.start();
    QVERIFY2(started.app != nullptr, "the application must be composed");
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY2(started.app->shell() != nullptr, "the composed application must expose the shell");
    QVERIFY2(started.app->lifecycle() != nullptr,
             "the composed application must expose the lifecycle controller");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    started.app->lifecycle()->onLogoutClearRequested();
    QApplication::processEvents(); // no gateway tick: no transfer can complete

    started.app->shutdown();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.statuses;

    // Non-vacuity: the clear must have produced a visible state before shutdown.
    int clearStatuses = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (isLogoutClear(status))
            ++clearStatuses;
    }
    QVERIFY2(clearStatuses >= 1,
             qPrintable(QStringLiteral("the pending logout clear produced no visible "
                                       "command state before shutdown; observed sequence: %1")
                            .arg(describeSequence(produced))));

    int fabricatedSuccesses = 0;
    QString fabricatedDetails;
    for (const OperatorCommandStatus &status : produced) {
        if (isSuccessOf(status)) {
            ++fabricatedSuccesses;
            fabricatedDetails += QStringLiteral("[%1]").arg(status.human_readable_detail);
        }
    }
    QVERIFY2(fabricatedSuccesses == 0,
             qPrintable(QStringLiteral("application shutdown with an unconfirmed logout "
                                       "clear reported %1 success status(es) %2; observed "
                                       "sequence: %3")
                            .arg(fabricatedSuccesses)
                            .arg(fabricatedDetails)
                            .arg(describeSequence(produced))));

    int succeededStates = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (isClearSuccess(status))
            ++succeededStates;
    }
    QVERIFY2(succeededStates == 0,
             qPrintable(QStringLiteral("application shutdown with an unconfirmed logout "
                                       "clear produced %1 Succeeded state(s); observed "
                                       "sequence: %2")
                            .arg(succeededStates)
                            .arg(describeSequence(produced))));
}

// --- OB-1 error behavior: a duplicate clear while pending is absorbed -----------
//
// Brief error_behaviors: "A duplicate logout-clear request while a clear is
// pending must not start a second lifecycle (exactly one terminal for the
// pending clear)."

void PlcHmi009LogoutClearTest::duplicateClearWhilePendingStartsNoSecondLifecycle()
{
    StartedApp started;
    started.start();
    QVERIFY2(started.app != nullptr, "the application must be composed");
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY2(started.app->shell() != nullptr, "the composed application must expose the shell");
    QVERIFY2(started.app->lifecycle() != nullptr,
             "the composed application must expose the lifecycle controller");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    started.app->lifecycle()->onLogoutClearRequested();
    QApplication::processEvents(); // pending: no transfer can complete yet

    int requestStarts = 0;
    for (const OperatorCommandStatus &status : sink.statuses) {
        if (isLogoutClear(status) && isRequestStart(status))
            ++requestStarts;
    }
    QVERIFY2(requestStarts >= 1,
             qPrintable(QStringLiteral("non-vacuity: the first clear must produce a visible "
                                       "request-start before the duplicate; observed: %1")
                            .arg(describeSequence(sink.statuses))));

    // Duplicate while the first clear is still pending/unconfirmed.
    started.app->lifecycle()->onLogoutClearRequested();
    QApplication::processEvents();

    // The pending clear (and only it) converges through the confirmation path.
    for (int i = 0; i < kConfirmedClearTicks; ++i) {
        started.gw->tick();
        QApplication::processEvents();
    }

    const QVector<OperatorCommandStatus> produced = sink.statuses;
    int terminals = 0;
    int successes = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (!isLogoutClear(status) || !isTerminal(status.lifecycle_state))
            continue;
        ++terminals;
        if (isClearSuccess(status))
            ++successes;
    }
    QVERIFY2(terminals == 1,
             qPrintable(QStringLiteral("the duplicate logout clear produced %1 terminal(s) "
                                       "instead of exactly one for the pending clear; observed "
                                       "sequence: %2")
                            .arg(terminals)
                            .arg(describeSequence(produced))));
    QVERIFY2(successes == 1,
             qPrintable(QStringLiteral("the confirmed duplicate logout clear produced %1 "
                                       "success terminal(s) instead of exactly one; observed "
                                       "sequence: %2")
                            .arg(successes)
                            .arg(describeSequence(produced))));

    QVERIFY2(!started.gw->model().readCoil(kM42),
             "the duplicate clear must still leave M42 off");
    for (int address = kFirstManualOutput; address <= kLastManualOutput; ++address) {
        QVERIFY2(!started.gw->model().readCoil(address),
                 qPrintable(QStringLiteral("the duplicate clear must still leave M%1 off")
                                .arg(address)));
    }

    started.shutdown();
}

// --- OB-1 boundary: the clear never changes M100 (estop) or M105 ----------------
//
// Brief boundary_cases: "M100 and M105 are never changed by the clear."
// Pre-state through the observable machine model: M105 (the address adjacent to
// the cleared manual outputs) energized, M100 (estop) off, and two of the
// clear's own addresses energized so the clear has observable work to do.

void PlcHmi009LogoutClearTest::clearLeavesM100AndM105Untouched()
{
    StartedApp started;
    started.start();
    QVERIFY2(started.app != nullptr, "the application must be composed");
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY2(started.app->shell() != nullptr, "the composed application must expose the shell");
    QVERIFY2(started.app->lifecycle() != nullptr,
             "the composed application must expose the lifecycle controller");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.gw->model().writeCoil(100, false); // M100 estop off
    started.gw->model().writeCoil(105, true);  // M105 must survive the clear
    started.gw->model().writeCoil(kM42, true); // M42 must be cleared
    started.gw->model().writeCoil(kFirstManualOutput, true); // M106 must be cleared

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    started.app->lifecycle()->onLogoutClearRequested();
    for (int i = 0; i < kConfirmedClearTicks; ++i) {
        started.gw->tick();
        QApplication::processEvents();
    }

    int clearStatuses = 0;
    for (const OperatorCommandStatus &status : sink.statuses) {
        if (isLogoutClear(status))
            ++clearStatuses;
    }
    QVERIFY2(clearStatuses >= 1,
             qPrintable(QStringLiteral("non-vacuity: the clear must produce a visible command "
                                       "state; observed sequence: %1")
                            .arg(describeSequence(sink.statuses))));

    // Never changed, in either direction.
    QVERIFY2(!started.gw->model().readCoil(100),
             "the clear must not energize M100 (estop)");
    QVERIFY2(started.gw->model().readCoil(105),
             "the clear must not clear M105; it only clears M42 and M106-M111");

    // The clear's own addresses are off after the confirmed clear.
    QVERIFY2(!started.gw->model().readCoil(kM42),
             "the clear must clear M42");
    for (int address = kFirstManualOutput; address <= kLastManualOutput; ++address) {
        QVERIFY2(!started.gw->model().readCoil(address),
                 qPrintable(QStringLiteral("the clear must clear M%1").arg(address)));
    }

    started.shutdown();
}

// --- PLC-HMI-011 OB-9: the clear also releases the home-start bit ---------------
//
// Brief OB-9: a logout or session-timeout clear while online writes the
// home-start bit (coil 50) to 0 together with the existing continuous-output
// clears, and reports exactly one success only when every write is confirmed.
// The clear still never touches the emergency-stop request (M100) or the
// passthrough mode bit (M105).

void PlcHmi009LogoutClearTest::clearReleasesTheHomeStartBitWithTheContinuousOutputs()
{
    StartedApp started;
    started.start();
    QVERIFY2(started.app != nullptr, "the application must be composed");
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY2(started.app->shell() != nullptr, "the composed application must expose the shell");
    QVERIFY2(started.app->lifecycle() != nullptr,
             "the composed application must expose the lifecycle controller");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    // Observable pre-state: a homing request is in progress (M50 energized),
    // the clear's own addresses are energized (M42, M106), and the two
    // protected bits are set to their non-default values so "untouched" is
    // observable in both directions (M100 on, M105 off).
    started.gw->model().writeCoil(kM50, true);
    started.gw->model().writeCoil(kM42, true);
    started.gw->model().writeCoil(kFirstManualOutput, true);
    started.gw->model().writeCoil(kM100, true);
    started.gw->model().writeCoil(kM105, false);
    QVERIFY2(started.gw->model().readCoil(kM50),
             "precondition: the home-start bit must be set before the clear");

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    // Record every submission touching M50 so the assertion cannot pass merely
    // because the simulated controller cleared its own start bit during the
    // ticks: the clear itself must have written the home-start bit.
    int homeStartSubmissions = 0;
    QObject submissionScope;
    QObject::connect(started.gw, &SimulatedPlcGateway::submissionCompleted, &submissionScope,
                     [&homeStartSubmissions](const SubmissionCompletion &completion) {
                         if (completion.address == kM50)
                             ++homeStartSubmissions;
                     });

    started.app->lifecycle()->onLogoutClearRequested();
    for (int i = 0; i < kConfirmedClearTicks; ++i) {
        started.gw->tick();
        QApplication::processEvents();
    }

    const QVector<OperatorCommandStatus> produced = sink.statuses;

    // Non-vacuity: the clear must produce a visible command state.
    int clearStatuses = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (isLogoutClear(status))
            ++clearStatuses;
    }
    QVERIFY2(clearStatuses >= 1,
             qPrintable(QStringLiteral("non-vacuity: the clear must produce a visible "
                                       "command state; observed sequence: %1")
                            .arg(describeSequence(produced))));

    // Exactly one success terminal for the confirmed clear, with a detail.
    int successCount = 0;
    for (const OperatorCommandStatus &status : produced) {
        if (isClearSuccess(status))
            ++successCount;
    }
    QVERIFY2(successCount == 1,
             qPrintable(QStringLiteral("the confirmed clear reported %1 success "
                                       "terminal(s) instead of exactly one; observed "
                                       "sequence: %2")
                            .arg(successCount)
                            .arg(describeSequence(produced))));

    // OB-9: the clear itself wrote the home-start bit to 0, together with the
    // continuous outputs. A mere absence of M50 is not sufficient evidence.
    QVERIFY2(homeStartSubmissions >= 1,
             "the confirmed clear submitted no home-start write at all");
    QVERIFY2(!started.gw->model().readCoil(kM50),
             "the confirmed clear must leave the home-start bit (M50) off");
    QVERIFY2(!started.gw->model().readCoil(kM42),
             "the confirmed clear must leave M42 off");
    for (int address = kFirstManualOutput; address <= kLastManualOutput; ++address) {
        QVERIFY2(!started.gw->model().readCoil(address),
                 qPrintable(QStringLiteral("the confirmed clear must leave M%1 off")
                                .arg(address)));
    }

    // The clear still never touches the emergency-stop request or the
    // passthrough mode bit, in either direction.
    QVERIFY2(started.gw->model().readCoil(kM100),
             "the clear must not clear the emergency-stop request M100");
    QVERIFY2(!started.gw->model().readCoil(kM105),
             "the clear must not energize M105");

    started.shutdown();
}

QTEST_MAIN(PlcHmi009LogoutClearTest)
#include "plc_hmi_009_logout_clear_test.moc"
