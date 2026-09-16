// PLC-HMI-006 black-box tests (independent): restricted-mode de-energizing
// release of a held manual output (brief OB-5) and the 50-400 mm target-width
// envelope on the operator surface (brief OB-6).
//
// Authored only from .ai/test-briefs/PLC-HMI-006.yaml, the approved
// .ai/project-contract.yaml, and inspectable test sources under tests/**. No
// production implementation source was read.
//
// Existing test-visible surface used (every symbol below appears in
// inspectable test sources under tests/**):
//   * Application/AppConfig (start/shutdown/gateway/shell/window/coordinator/
//     database) and the production restricted-mode entry: an unusable SQLite
//     file at startup (spec section 13), established by
//     tests/integration/restricted_mode_routing_test.cpp;
//   * SimulatedPlcGateway (tick/isOnline/model()/lastSnapshot/
//     submissionCompleted) as the parity path;
//   * ControlCoordinator manual commands manualHold(address, value) (value
//     false is the de-energizing write 0), manualLatch, bypass, and the
//     command result {accepted, reason};
//   * ShellModel::operatorCommandStatusChanged / operatorCommandStatus and
//     the domain OperatorCommandStatus lifecycle incl. isTerminal;
//   * RecipeWidthPage::applyAdjustRequested(int), widthSpin(), applyButton(),
//     statusText() and the two-step apply confirm used by the inspectable
//     PLC-HMI-001/002/008 tests.
//
// Controller adjudication frozen for OB-5 (recorded in
// .ai/reports/PLC-HMI-006-test-red.yaml): restricted-mode entry must clear
// continuous outputs and release UI hold intents through the production
// lifecycle seam LifecycleController::enterRestrictedMode(reason), which is
// public and callable after Application::start() with a real held state;
// LifecycleController is reached through Application::lifecycle(). The
// release-direction exemption was rejected as an architect-only widening, so
// the restricted command set is not exercised for a release command: the clear
// happens at entry and must not be blocked by the restricted gate. Exact
// rejection wording is not fixed by the brief and is therefore not asserted.
//
// Expected RED until PLC-HMI-006 is implemented (runtime assertions, not a
// compile error): no 50-400 mm operator envelope exists, so out-of-envelope
// targets are accepted and written to D128 and the parity path has no 50-400
// precondition; and the existing public lifecycle seam
// Application::lifecycle()->enterRestrictedMode(reason) currently performs
// only a logout, so restricted-mode entry does not clear holds/outputs and the
// OB-5 release assertions fail. Fresh-session execution evidence (moc +
// compiler probe, ctest listing; the sandbox has no registered PLC-HMI-006
// target) is recorded in .ai/reports/PLC-HMI-006-test-red.yaml.

#include <QtTest>

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSpinBox>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "application/control_coordinator.h"
#include "app/lifecycle_controller.h"
#include "domain/operator_command_status.h"
#include "ports/iplc_gateway.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM103 = 103; // reset / home pulse
constexpr quint16 kM106 = 106; // manual hold output
constexpr quint16 kM109 = 109; // manual latch output
constexpr quint16 kM110 = 110; // bypass output
constexpr quint16 kD128 = 128; // target width
constexpr quint16 kD130 = 130; // current width

// Brief OB-6: the operator target-width envelope.
constexpr int kMinTargetWidthMm = 50;
constexpr int kMaxTargetWidthMm = 400;

// Simulator default target width of a fresh machine, documented by the
// inspectable PLC-HMI-008 test source (restricted_mode_routing_test.cpp).
constexpr quint16 kDefaultTargetWidth = 200;

// Deterministic current width used for the envelope cases so the +/-350 mm
// decoded delta window cannot confound the envelope decision.
constexpr quint16 kDeterministicCurrentWidth = 100;

constexpr int kMaxConvergenceTicks = 45;

// --- composition fixture -------------------------------------------------------

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
    // database (open/migration failure, spec section 13).
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

    // Inspectable offline injection: the simulator's link-down fault surface
    // plus bounded ticks, the same convention as
    // tests/unit/plc_submission_identity_test.cpp. Offline submissions are
    // rejected without a request id and with a non-empty reason, so every
    // logout-clear write is unconfirmable in this state.
    void advanceUntilOffline(int maxTicks = 20)
    {
        if (gw == nullptr)
            return;
        gw->setLinkDown(true);
        for (int i = 0; i < maxTicks && gw->isOnline(); ++i)
            gw->tick();
    }
};

// --- shared helpers -------------------------------------------------------------

// Counts completed PLC submissions to one protocol address.
int countSubmissionsTo(const QVector<quint16> &submittedAddresses, quint16 address)
{
    int count = 0;
    for (const quint16 submitted : submittedAddresses) {
        if (submitted == address)
            ++count;
    }
    return count;
}

// True when every produced status is a terminal state (nothing left pending).
bool allTerminal(const QVector<OperatorCommandStatus> &produced)
{
    for (const OperatorCommandStatus &status : produced) {
        if (!isTerminal(status.lifecycle_state))
            return false;
    }
    return true;
}

bool anyRejectionWithReason(const QVector<OperatorCommandStatus> &produced)
{
    for (const OperatorCommandStatus &status : produced) {
        if (status.lifecycle_state == OperatorCommandState::Rejected
            && !status.human_readable_detail.trimmed().isEmpty()) {
            return true;
        }
    }
    return false;
}

bool anyVisibleReason(const QVector<OperatorCommandStatus> &produced)
{
    for (const OperatorCommandStatus &status : produced) {
        if (!status.human_readable_detail.trimmed().isEmpty())
            return true;
    }
    return false;
}

// True when a status is a non-success terminal state, i.e. the command failed
// observably instead of being reported as a machine success.
bool isFailureTerminal(const OperatorCommandStatus &status)
{
    return isTerminal(status.lifecycle_state)
        && status.lifecycle_state != OperatorCommandState::Succeeded;
}

bool anyFailureTerminalWithDetail(const QVector<OperatorCommandStatus> &produced)
{
    for (const OperatorCommandStatus &status : produced) {
        if (isFailureTerminal(status) && !status.human_readable_detail.trimmed().isEmpty())
            return true;
    }
    return false;
}

// Bounded tick loop: ticks the simulator until the predicate holds or the bound
// is exhausted, then reports the final predicate state.
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

// Drives the reset/home path (M103 pulse; the home return takes 2 s). Follows
// the inspectable convention of tests/integration/restricted_mode_routing_test.cpp;
// no home-completion address is invented here.
void homeReady(StartedApp &started)
{
    if (started.gw == nullptr)
        return;
    started.gw->model().writeCoil(kM103, true);
    started.gw->model().writeCoil(kM103, false);
    started.gw->tick();
    started.gw->tick(); // home return takes 2 s
}

// Collects shell-projected operator command statuses for the assertions below.
struct StatusSink
{
    QVector<OperatorCommandStatus> statuses;

    int mark() const { return int(statuses.size()); }
    QVector<OperatorCommandStatus> since(int mark) const { return statuses.mid(mark); }
};

void attachStatusSink(QObject *context, ShellModel *shell, StatusSink &sink)
{
    QObject::connect(shell, &ShellModel::operatorCommandStatusChanged, context,
                     [&sink](const OperatorCommandStatus &status) {
                         sink.statuses.append(status);
                     });
}

// Attaches a submission log to the simulator gateway parity path.
void attachSubmissionLog(QObject *context, SimulatedPlcGateway *gateway,
                         QVector<quint16> &submittedAddresses)
{
    QObject::connect(gateway, &SimulatedPlcGateway::submissionCompleted, context,
                     [&submittedAddresses](const SubmissionCompletion &completion) {
                         submittedAddresses.append(completion.address);
                     });
}

} // namespace

class PlcHmi006ReleaseAndEnvelopeTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-5: restricted-mode entry releases a held output -------------------
    void restrictedEntryClearsHeldOutputAndConvergesToTerminal();
    void restrictedEntryClearsHeldLatchAndConvergesPendingConfirmationTerminally();
    void restrictedEntryRejectsHeldOutputReenergizingWithoutLatch();
    void restrictedEntryWhileOfflineNeverReportsTheClearSucceeded();

    // --- OB-6: the 50-400 mm operator target-width envelope -------------------
    void outOfEnvelopeAdjustTargetsAreRejectedWithoutSubmission();
    void inEnvelopeAdjustTargetsConvergeVisibly();
    void widthSpinOutOfEnvelopeAttemptIsNeverSilentlyAccepted();
};

// --- OB-5: restricted-mode entry releases a held output -------------------------

void PlcHmi006ReleaseAndEnvelopeTest::restrictedEntryClearsHeldOutputAndConvergesToTerminal()
{
    // Brief OB-5: entering restricted mode must actively de-energize an output
    // that is currently held, release the UI hold intent, leave no command
    // pending, and converge in bounded time. The restricted-mode gate does not
    // apply to this release: the clear happens at entry.
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
    QVector<quint16> submittedAddresses;
    QObject submissionScope; // RAII: severs the connection before the log dies
    attachSubmissionLog(&submissionScope, started.gw, submittedAddresses);

    // Healthy, unrestricted, homed and held output: the allowed path.
    homeReady(started);
    started.app->coordinator()->manualHold(kM106, true);
    QVERIFY2(tickUntil(started, 20, [&] { return started.gw->model().readCoil(kM106); }),
             "precondition: the manual hold must energize M106 while unrestricted");

    const int mark = sink.mark();
    started.app->lifecycle()->enterRestrictedMode(QStringLiteral("test"));

    // The held output must be cleared and stay cleared over bounded ticks.
    QVERIFY2(tickUntil(started, kMaxConvergenceTicks,
                       [&] { return !started.gw->model().readCoil(kM106); }),
             "restricted-mode entry must clear the held output (M106 must drop)");
    for (int i = 0; i < 10; ++i)
        started.gw->tick();
    QApplication::processEvents();
    QVERIFY2(!started.gw->model().readCoil(kM106),
             "the released output must not re-energize after the release converged");

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "the release of a held output must produce a visible command state");
    QVERIFY2(allTerminal(produced),
             "no command may be left pending after restricted-mode entry");
    QVERIFY2(!started.gw->model().readCoil(kM106),
             "the release must not leave the output energized");

    started.shutdown();
}

void PlcHmi006ReleaseAndEnvelopeTest::restrictedEntryClearsHeldLatchAndConvergesPendingConfirmationTerminally()
{
    // Brief OB-5, latch half: a held manual LATCH (M109) whose confirmation may
    // still be outstanding when restricted mode is entered must not leave a
    // dangling pending hold state. The entry clear happens at entry (the
    // release-direction exemption is rejected), so this mirrors the manual-hold
    // case above for the latch family and enters immediately after the latch
    // energizes, which keeps any still-pending latch confirmation in scope:
    // every status produced from entry on must be terminal, and the latch
    // output must clear and stay clear.
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
    QVector<quint16> submittedAddresses;
    QObject submissionScope; // RAII: severs the connection before the log dies
    attachSubmissionLog(&submissionScope, started.gw, submittedAddresses);

    // Healthy, unrestricted, homed and latched: the allowed path.
    homeReady(started);
    started.app->coordinator()->manualLatch(kM109, true);
    QVERIFY2(tickUntil(started, 20, [&] { return started.gw->model().readCoil(kM109); }),
             "precondition: the manual latch must energize M109 while unrestricted");

    const int mark = sink.mark();
    started.app->lifecycle()->enterRestrictedMode(QStringLiteral("test"));

    // The held latch must be cleared and stay cleared over bounded ticks.
    QVERIFY2(tickUntil(started, kMaxConvergenceTicks,
                       [&] { return !started.gw->model().readCoil(kM109); }),
             "restricted-mode entry must clear the held latch (M109 must drop)");
    for (int i = 0; i < 10; ++i)
        started.gw->tick();
    QApplication::processEvents();
    QVERIFY2(!started.gw->model().readCoil(kM109),
             "the released latch must not re-energize after the release converged");

    // No dangling latch confirmation: every status produced from entry on is
    // terminal, and the release itself is visible in the UI state.
    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "the release of a held latch must produce a visible command state");
    QVERIFY2(allTerminal(produced),
             "no pending latch confirmation may remain dangling after restricted-mode entry");
    QVERIFY2(!started.gw->model().readCoil(kM109),
             "the release must not leave the latch energized");

    started.shutdown();
}

void PlcHmi006ReleaseAndEnvelopeTest::restrictedEntryRejectsHeldOutputReenergizingWithoutLatch()
{
    // Brief OB-5 second half: the release must not be a one-off side effect
    // that leaves the restricted command set open; a further hold press in
    // restricted mode is rejected with a visible reason, and the repeated
    // attempt produces no latch and no later state.
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY(started.app->coordinator() != nullptr);
    QVERIFY(started.app->lifecycle() != nullptr);
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Admin);
    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);
    QVector<quint16> submittedAddresses;
    QObject submissionScope; // RAII: severs the connection before the log dies
    attachSubmissionLog(&submissionScope, started.gw, submittedAddresses);

    homeReady(started);
    started.app->coordinator()->manualHold(kM106, true);
    QVERIFY2(tickUntil(started, 20, [&] { return started.gw->model().readCoil(kM106); }),
             "precondition: the manual hold must energize M106 while unrestricted");

    started.app->lifecycle()->enterRestrictedMode(QStringLiteral("test"));
    QVERIFY2(tickUntil(started, kMaxConvergenceTicks,
                       [&] { return !started.gw->model().readCoil(kM106); }),
             "precondition: restricted-mode entry must clear the held output");

    // Drain the entry-clear write completions deterministically: the simulator
    // applies coil writes to the model synchronously at submit time, so the
    // "M106 dropped" convergence above does not imply the release submissions
    // have been observed. Without this drain their submissionCompleted signals
    // would be miscounted as if the restricted press had produced them.
    for (int i = 0; i < 10; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const int mark = sink.mark();
    const int submissionsBefore = int(submittedAddresses.size());
    started.app->coordinator()->manualHold(kM106, true);
    for (int i = 0; i < 6; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "a restricted-mode hold attempt must produce a visible command state");
    QVERIFY2(anyRejectionWithReason(produced),
             "a restricted-mode hold attempt must be visibly rejected with a reason");
    QVERIFY2(!started.gw->model().readCoil(kM106),
             "no hold attempt in restricted mode may re-energize the released output");
    QCOMPARE(int(submittedAddresses.size()), submissionsBefore);

    // No latch: the state settles bounded, and a repeated press is rejected
    // again with its own reason instead of being swallowed.
    const int settled = sink.mark();
    for (int i = 0; i < 30; ++i)
        started.gw->tick();
    QApplication::processEvents();
    QCOMPARE(sink.mark(), settled);

    const int secondMark = sink.mark();
    started.app->coordinator()->manualHold(kM106, true);
    for (int i = 0; i < 6; ++i)
        started.gw->tick();
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> second = sink.since(secondMark);
    QVERIFY2(!second.isEmpty(),
             "a repeated restricted-mode hold attempt must produce a fresh visible state");
    QVERIFY2(anyRejectionWithReason(second),
             "a repeated restricted-mode hold attempt must carry a fresh non-empty reason");
    QVERIFY2(!started.gw->model().readCoil(kM106),
             "the repeated restricted attempt must not energize the output");

    started.shutdown();
}

void PlcHmi006ReleaseAndEnvelopeTest::restrictedEntryWhileOfflineNeverReportsTheClearSucceeded()
{
    // NF-03 lock: with the PLC link down the entry clear cannot be confirmed
    // by the machine, so restricted-mode entry must never report the clear as
    // a machine success. The gateway rejects offline submissions, so every
    // produced status must be terminal, none may be Succeeded, and the failure
    // must be visible to the operator with a reason.
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    QVERIFY(started.app->shell() != nullptr);
    QVERIFY(started.app->coordinator() != nullptr);
    QVERIFY(started.app->lifecycle() != nullptr);

    // Inspectable offline injection: the simulator's link-down fault surface
    // plus bounded ticks. The application is not advanced until online, so
    // every write submitted by the entry clear is unconfirmable.
    started.advanceUntilOffline();
    QVERIFY2(!started.gw->isOnline(), "precondition: the simulator gateway must be offline");

    // The role request may itself be refused while the session is not fully
    // online; it is not the subject of this case.
    started.app->coordinator()->setRole(Role::Admin);

    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);
    QVector<quint16> submittedAddresses;
    QObject submissionScope; // RAII: severs the connection before the log dies
    attachSubmissionLog(&submissionScope, started.gw, submittedAddresses);

    const int mark = sink.mark();
    started.app->lifecycle()->enterRestrictedMode(QStringLiteral("test"));
    for (int i = 0; i < 10; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "restricted-mode entry while offline must still produce a visible command state");
    QVERIFY2(allTerminal(produced),
             "no command may be left dangling when restricted-mode entry runs offline");
    for (const OperatorCommandStatus &status : produced) {
        QVERIFY2(status.lifecycle_state != OperatorCommandState::Succeeded,
                 qPrintable(QStringLiteral("the offline entry clear reported %1 instead of a "
                                           "failure visible to the operator")
                                .arg(toString(status.lifecycle_state))));
    }
    QVERIFY2(anyFailureTerminalWithDetail(produced),
             "an unconfirmable offline clear must surface a failure terminal with a reason");
    const bool failedVisibly = [&produced] {
        for (const OperatorCommandStatus &status : produced) {
            const bool failureState =
                status.lifecycle_state == OperatorCommandState::CommunicationsLost
                || status.lifecycle_state == OperatorCommandState::Failed;
            if (failureState && !status.human_readable_detail.trimmed().isEmpty())
                return true;
        }
        return false;
    }();
    QVERIFY2(failedVisibly,
             "the offline clear failure must be reported as CommunicationsLost or Failed with "
             "a non-empty human_readable_detail");

    started.shutdown();
}

// --- OB-6: the 50-400 mm operator target-width envelope -------------------------

void PlcHmi006ReleaseAndEnvelopeTest::outOfEnvelopeAdjustTargetsAreRejectedWithoutSubmission()
{
    // Brief OB-6: operator targets outside 50..400 mm must be rejected visibly
    // and must never reach the PLC. The simulator default target width stays
    // untouched and D128 is never written.
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Admin);
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(recipe != nullptr);
    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);
    QVector<quint16> submittedAddresses;
    QObject submissionScope; // RAII: severs the connection before the log dies
    attachSubmissionLog(&submissionScope, started.gw, submittedAddresses);

    // Deterministic current width so a decoded delta cannot confound the
    // envelope decision.
    started.gw->model().writeRegister(kD130, kDeterministicCurrentWidth);
    QVERIFY2(started.gw->model().readRegister(kD128) == kDefaultTargetWidth,
             "precondition: the fresh machine starts at the default target width");

    const int targets[] = {kMinTargetWidthMm - 1, kMaxTargetWidthMm + 1};
    for (const int target : targets) {
        const int mark = sink.mark();
        const int submissionsBefore = int(submittedAddresses.size());
        const quint16 d128Before = started.gw->model().readRegister(kD128);
        emit recipe->applyAdjustRequested(target);
        for (int i = 0; i < 6; ++i)
            started.gw->tick();
        QApplication::processEvents();

        const QVector<OperatorCommandStatus> produced = sink.since(mark);
        QVERIFY2(!produced.isEmpty(),
                 qPrintable(QStringLiteral("the out-of-envelope target %1 produced no "
                                           "visible command state")
                                .arg(target)));
        for (const OperatorCommandStatus &status : produced) {
            QVERIFY2(status.lifecycle_state == OperatorCommandState::Rejected,
                     qPrintable(QStringLiteral("the out-of-envelope target %1 produced "
                                               "state %2 instead of a rejection")
                                    .arg(target)
                                    .arg(toString(status.lifecycle_state))));
            QVERIFY2(!status.human_readable_detail.trimmed().isEmpty(),
                     qPrintable(QStringLiteral("the out-of-envelope target %1 rejection "
                                               "carries no visible reason")
                                    .arg(target)));
        }
        QCOMPARE(int(submittedAddresses.size()), submissionsBefore);
        QCOMPARE(int(countSubmissionsTo(submittedAddresses, kD128)), 0);
        QVERIFY2(started.gw->model().readRegister(kD128) == d128Before,
                 qPrintable(QStringLiteral("the out-of-envelope target %1 must not write "
                                           "D128")
                                .arg(target)));
    }

    QVERIFY2(started.gw->model().readRegister(kD128) == kDefaultTargetWidth,
             "out-of-envelope operator targets must not replace the machine target");

    // No latch: further ticks produce no accepted/pending or later state.
    const int settled = sink.mark();
    for (int i = 0; i < 30; ++i)
        started.gw->tick();
    QApplication::processEvents();
    QCOMPARE(sink.mark(), settled);

    started.shutdown();
}

void PlcHmi006ReleaseAndEnvelopeTest::inEnvelopeAdjustTargetsConvergeVisibly()
{
    // Brief OB-6 boundary: the exact envelope endpoints 50 and 400 mm must not
    // be treated as out of range. A correct implementation either accepts them
    // and converges to a terminal state with D128 equal to the request, or
    // rejects them visibly with a reason; it must never silently ignore them.
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Admin);
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(recipe != nullptr);
    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);
    QVector<quint16> submittedAddresses;
    QObject submissionScope; // RAII: severs the connection before the log dies
    attachSubmissionLog(&submissionScope, started.gw, submittedAddresses);

    started.gw->model().writeRegister(kD130, kDeterministicCurrentWidth);

    const int targets[] = {kMinTargetWidthMm, kMaxTargetWidthMm};
    for (const int target : targets) {
        const int mark = sink.mark();
        emit recipe->applyAdjustRequested(target);
        QVERIFY2(tickUntil(started, kMaxConvergenceTicks,
                           [&] { return !sink.since(mark).isEmpty(); }),
                 qPrintable(QStringLiteral("the in-envelope target %1 produced no visible "
                                           "command state")
                                .arg(target)));
        tickUntil(started, kMaxConvergenceTicks,
                  [&] { return allTerminal(sink.since(mark)); });

        const QVector<OperatorCommandStatus> produced = sink.since(mark);
        const bool accepted = [&] {
            for (const OperatorCommandStatus &status : produced) {
                if (status.lifecycle_state != OperatorCommandState::Rejected)
                    return true;
            }
            return false;
        }();
        if (accepted) {
            QVERIFY2(allTerminal(produced),
                     qPrintable(QStringLiteral("the accepted in-envelope target %1 must "
                                               "converge to a terminal state")
                                        .arg(target)));
            QVERIFY2(started.gw->model().readRegister(kD128) == quint16(target),
                     qPrintable(QStringLiteral("the accepted in-envelope target %1 must "
                                               "reach D128")
                                        .arg(target)));
        } else {
            QVERIFY2(anyVisibleReason(produced),
                     qPrintable(QStringLiteral("the in-envelope target %1 was not accepted "
                                               "and carries no visible reason")
                                        .arg(target)));
        }
    }

    started.shutdown();
}

void PlcHmi006ReleaseAndEnvelopeTest::widthSpinOutOfEnvelopeAttemptIsNeverSilentlyAccepted()
{
    // Brief OB-6: the same envelope applies to the widget path. The operator
    // cannot prepare an out-of-envelope target and have it accepted by the
    // two-step apply. Refusal has two allowed shapes: (a) the control itself
    // refuses the out-of-envelope entry (e.g. clamps it into the range), so the
    // attempt never reaches the application and no operator rejection status
    // is required; (b) the control does not refuse the entry, and then the
    // apply attempt must produce a visible rejection with a reason. In both
    // shapes D128 keeps the machine target and the attempt is never silently
    // accepted.
    StartedApp started;
    started.start();
    QVERIFY(started.app != nullptr);
    QVERIFY2(started.gw != nullptr, "the composed application must expose the gateway");
    started.advanceUntilOnline();
    QVERIFY2(started.gw->isOnline(), "precondition: the simulator gateway must come online");

    started.app->coordinator()->setRole(Role::Admin);
    RecipeWidthPage *recipe = started.app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(recipe != nullptr);
    QSpinBox *widthSpin = recipe->widthSpin();
    QVERIFY(widthSpin != nullptr);
    QVERIFY(recipe->applyButton() != nullptr);
    StatusSink sink;
    QObject sinkScope; // RAII: severs the connection before the sink dies
    attachStatusSink(&sinkScope, started.app->shell(), sink);
    QVector<quint16> submittedAddresses;
    QObject submissionScope; // RAII: severs the connection before the log dies
    attachSubmissionLog(&submissionScope, started.gw, submittedAddresses);

    const int entered = kMaxTargetWidthMm + 1;
    widthSpin->setValue(entered);
    const int displayed = widthSpin->value();
    const int mark = sink.mark();
    const int submissionsBefore = int(submittedAddresses.size());
    const quint16 d128Before = started.gw->model().readRegister(kD128);

    recipe->applyButton()->click();
    for (int i = 0; i < 4; ++i)
        started.gw->tick();
    QApplication::processEvents();
    recipe->applyButton()->click();
    for (int i = 0; i < 6; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    const quint16 d128After = started.gw->model().readRegister(kD128);

    // Never silently accept an out-of-envelope value: D128 must never become
    // the entered 401 mm, and any applied target must be the value the control
    // visibly displayed at apply time.
    QVERIFY2(d128After != quint16(entered),
             "the widget path must never apply an out-of-envelope target to D128");
    if (d128After != d128Before) {
        QVERIFY2(d128After == quint16(displayed),
                 "an applied widget target must be the value the operator could see");
        QVERIFY2(d128After >= quint16(kMinTargetWidthMm)
                     && d128After <= quint16(kMaxTargetWidthMm),
                 "an applied widget target must lie inside the 50-400 mm envelope");
    } else {
        QCOMPARE(int(submittedAddresses.size()), submissionsBefore);
        QCOMPARE(int(countSubmissionsTo(submittedAddresses, kD128)), 0);

        // Refusal shape (a): the control refused the out-of-envelope entry and
        // is showing a different, in-envelope value (e.g. a clamp to the range
        // maximum). The attempt never reached the application, so no operator
        // rejection status is expected; the never-silent property is carried by
        // the control refusal itself plus the D128/submission assertions above.
        const bool controlRefusedEntry =
            displayed != entered && displayed >= kMinTargetWidthMm
            && displayed <= kMaxTargetWidthMm;
        if (controlRefusedEntry) {
            QVERIFY2(displayed >= kMinTargetWidthMm && displayed <= kMaxTargetWidthMm,
                     "a control-refused out-of-envelope entry must leave the control "
                     "showing a value inside the 50-400 mm envelope");
        } else {
            // Refusal shape (b): the control did not refuse the entry, so the
            // apply attempt must not be silently ignored - a rejection with a
            // visible reason is required.
            QVERIFY2(anyRejectionWithReason(produced),
                     "an out-of-envelope widget attempt that is not applied must be "
                     "visibly rejected with a reason");
        }
    }
    QVERIFY2(allTerminal(produced),
             "the widget out-of-envelope attempt must not be left pending");

    started.shutdown();
}

QTEST_MAIN(PlcHmi006ReleaseAndEnvelopeTest)
#include "plc_hmi_006_release_and_envelope_test.moc"
