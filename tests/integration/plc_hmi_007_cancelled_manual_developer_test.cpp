// PLC-HMI-007 developer-owned regression test (D6-l): a pending manual
// confirmation cancelled by restricted-mode entry leaves the coordinator with
// no dangling submission bookkeeping and produces exactly one terminal outcome
// for that command generation (OB-7: never a double result).
//
// Authoring note: unlike the independent wave-2 target
// (tests/integration/plc_hmi_007_cancelled_manual_test.cpp) this is a
// developer-owned white-box lock. It intentionally asserts the terminal count
// per generation and the absence of a late second terminal after the
// cancellation, and every failure message carries the observed status
// sequence for triage.
//
// Terminal outcomes are counted per command generation, never globally:
// restricted-mode entry during a pending manual hold legitimately yields
// Failed(gen N, the cancelled hold) followed by Succeeded(gen N+1, the
// LogoutClear lifecycle the restricted-mode entry starts itself). A later
// generation is a new lifecycle, not a double result for the cancelled hold.

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
#include "app/lifecycle_controller.h"
#include "application/control_coordinator.h"
#include "domain/operator_command_status.h"
#include "ui/MainWindow.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

constexpr quint16 kM103 = 103; // reset / home pulse
constexpr quint16 kM106 = 106; // manual hold output

constexpr int kMaxTicks = 45;
constexpr int kDrainTicks = 20;

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

bool isRequestStart(const OperatorCommandStatus &status)
{
    return status.lifecycle_state == OperatorCommandState::Accepted
        || status.lifecycle_state == OperatorCommandState::Pending;
}

int terminalCountForGeneration(const QVector<OperatorCommandStatus> &statuses,
                               quint64 generation)
{
    int count = 0;
    for (const OperatorCommandStatus &status : statuses) {
        if (isTerminal(status.lifecycle_state) && status.command_generation == generation)
            ++count;
    }
    return count;
}

// Generations carrying more than one terminal outcome, in first-seen order.
QVector<quint64> generationsWithMultipleTerminals(
    const QVector<OperatorCommandStatus> &statuses)
{
    QVector<quint64> offenders;
    for (const OperatorCommandStatus &status : statuses) {
        if (!isTerminal(status.lifecycle_state))
            continue;
        const quint64 generation = status.command_generation;
        if (terminalCountForGeneration(statuses, generation) > 1
            && !offenders.contains(generation)) {
            offenders.append(generation);
        }
    }
    return offenders;
}

QString describeGenerations(const QVector<quint64> &generations)
{
    QStringList parts;
    for (quint64 generation : generations)
        parts.append(QString::number(generation));
    return parts.join(QStringLiteral(", "));
}

QString describeSequence(const QVector<OperatorCommandStatus> &statuses)
{
    QStringList parts;
    int index = 0;
    for (const OperatorCommandStatus &status : statuses) {
        parts.append(QStringLiteral("#%1 %2 gen=%3 detail='%4'")
                         .arg(index++)
                         .arg(toString(status.lifecycle_state))
                         .arg(status.command_generation)
                         .arg(status.human_readable_detail));
    }
    return parts.join(QStringLiteral(" | "));
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

void homeReady(StartedApp &started)
{
    if (started.gw == nullptr)
        return;
    started.gw->model().writeCoil(kM103, true);
    started.gw->model().writeCoil(kM103, false);
    started.gw->tick();
    started.gw->tick();
}

} // namespace

class PlcHmi007CancelledManualDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void restrictedEntryCancelsPendingManualHoldWithOneTerminalPerGeneration();
};

void PlcHmi007CancelledManualDeveloperTest::
    restrictedEntryCancelsPendingManualHoldWithOneTerminalPerGeneration()
{
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
    QObject sinkScope;
    attachStatusSink(&sinkScope, started.app->shell(), sink);

    homeReady(started);
    const int mark = sink.mark();
    started.app->coordinator()->manualHold(kM106, true);
    QVERIFY2(tickUntil(started, 20, [&] { return started.gw->model().readCoil(kM106); }),
             "precondition: the manual hold must energize M106 while unrestricted");

    started.app->lifecycle()->enterRestrictedMode(QStringLiteral("developer regression"));

    for (int i = 0; i < kMaxTicks; ++i)
        started.gw->tick();
    QApplication::processEvents();
    const QVector<OperatorCommandStatus> afterConvergence = sink.since(mark);
    QVERIFY2(!afterConvergence.isEmpty(),
             "the cancelled manual confirmation must produce a visible command state");

    // The hold's own generation: the first request start emitted after the
    // mark (fallback: the first status), i.e. the lifecycle the manualHold
    // opened. Later generations belong to lifecycles restricted-mode entry
    // starts on its own and are checked separately.
    quint64 attemptGeneration = afterConvergence.first().command_generation;
    for (const OperatorCommandStatus &status : afterConvergence) {
        if (isRequestStart(status)) {
            attemptGeneration = status.command_generation;
            break;
        }
    }

    QVERIFY2(terminalCountForGeneration(afterConvergence, attemptGeneration) == 1,
             qPrintable(QStringLiteral("the cancelled confirmation (generation %1) had "
                                       "not settled to exactly one terminal within the "
                                       "convergence window; sequence: %2")
                            .arg(attemptGeneration)
                            .arg(describeSequence(afterConvergence))));
    for (int i = 0; i < kDrainTicks; ++i)
        started.gw->tick();
    QApplication::processEvents();

    const QVector<OperatorCommandStatus> produced = sink.since(mark);
    QVERIFY2(!produced.isEmpty(),
             "the cancelled manual confirmation must produce a visible command state");

    // One terminal per generation is the approved design: no generation may
    // carry two terminal outcomes (OB-7), regardless of how many distinct
    // lifecycles the window contains.
    const QVector<quint64> multiTerminalGenerations =
        generationsWithMultipleTerminals(produced);
    QVERIFY2(multiTerminalGenerations.isEmpty(),
             qPrintable(QStringLiteral("generation(s) [%1] produced more than one terminal "
                                       "outcome, so the closure invariant is broken; "
                                       "sequence: %2")
                            .arg(describeGenerations(multiTerminalGenerations))
                            .arg(describeSequence(produced))));

    // OB-7 closure for the cancelled hold itself: after its terminal outcome no
    // further status may arrive for that generation (no re-issued request and no
    // second terminal). The gen+1 LogoutClear lifecycle is a legitimate new
    // generation and is deliberately not flagged here.
    bool attemptTerminalSeen = false;
    for (const OperatorCommandStatus &status : produced) {
        if (status.command_generation != attemptGeneration)
            continue;
        if (attemptTerminalSeen) {
            QVERIFY2(false,
                     qPrintable(QStringLiteral("status for the cancelled hold's "
                                               "generation %1 arrived after its terminal "
                                               "outcome: %2 %3; sequence: %4")
                                    .arg(attemptGeneration)
                                    .arg(toString(status.lifecycle_state))
                                    .arg(status.human_readable_detail)
                                    .arg(describeSequence(produced))));
        }
        if (isTerminal(status.lifecycle_state))
            attemptTerminalSeen = true;
    }
    QVERIFY2(attemptTerminalSeen,
             qPrintable(QStringLiteral("the cancelled hold's generation %1 never reached a "
                                       "terminal outcome; sequence: %2")
                            .arg(attemptGeneration)
                            .arg(describeSequence(produced))));

    started.shutdown();
}

QTEST_MAIN(PlcHmi007CancelledManualDeveloperTest)
#include "plc_hmi_007_cancelled_manual_developer_test.moc"
