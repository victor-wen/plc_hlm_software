// PLC-HMI-001 developer tests (D12): ControlCoordinator command lifecycle,
// the ShellModel OperatorCommandStatus projection and the ActionBar status
// label. These complement (never replace) the independent black-box tests in
// operator_command_lifecycle_test.cpp / command_status_projection_test.cpp.

#include <QtTest>
#include <QLabel>
#include <QSet>
#include <QSignalSpy>
#include <QVector>

#include <functional>
#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "application/control_coordinator.h"
#include "domain/operator_command_status.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching AddressTable).
constexpr quint16 kM50 = 50;
constexpr quint16 kM101 = 101;
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kM109 = 109;
constexpr quint16 kM110 = 110;

quint64 nextRequestId()
{
    static quint64 next = 1;
    return next++;
}

SubmissionResult acceptedResult()
{
    SubmissionResult r;
    r.accepted = true;
    r.request_id = nextRequestId();
    r.gateway_generation = 1;
    return r;
}

SubmissionResult rejectedResult(const QString &reason)
{
    SubmissionResult r;
    r.accepted = false;
    r.request_id = 0;
    r.gateway_generation = 1;
    r.immediate_rejection_reason = reason;
    return r;
}

// PLC-HMI-011 D6: the M103 pulse no longer starts homing; the single
// sustained M50=1 home-start write does.
void homeReady(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.model().writeCoil(kM50, true);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

void putInAutoMode(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM104, true);
    gw.tick();
}

ControlCoordinator::PulseTransport gatewayTransport(SimulatedPlcGateway &gw)
{
    ControlCoordinator::PulseTransport t;
    t.startPulse = [&gw](quint16 a) -> SubmissionResult {
        gw.model().writeCoil(a, true);
        gw.model().writeCoil(a, false);
        return acceptedResult();
    };
    t.writeHold = [&gw](quint16 a, bool v) -> SubmissionResult {
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) -> SubmissionResult {
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) -> SubmissionResult {
        gw.model().writeRegister(a, v);
        return acceptedResult();
    };
    return t;
}

ControlCoordinator *wire(SimulatedPlcGateway &gw, qint64 &now,
                         ControlCoordinator::PulseTransport t)
{
    auto *c = new ControlCoordinator(t, ControlCoordinator::Config(),
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

} // namespace

class OperatorLifecycleDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void duplicateRejectionForEveryEntryPoint();
    void manualAndBypassConfirmOnlyFromSnapshot();
    void manualConfirmTimeoutConvergesViaInjectedClock();
    void transportRejectionIsVisibleWithoutSuccess();
    void estopReleaseNamesPhysicalEstopWhenM0StillHeld();
    void lifecycleHelpersClassifyStates();
    void shellModelStoresProjectsAndClearsStatus();
    void actionBarRendersProjectedStatus();

private:
    void verifyRejected(QSignalSpy &rejected, Command cmd);
};

void OperatorLifecycleDeveloperTest::verifyRejected(QSignalSpy &rejected, Command cmd)
{
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), cmd);
    QVERIFY2(!rejected[0][1].toString().isEmpty(),
             "a visible rejection must always carry a reason");
}

// D3: every duplicate/in-progress request for the ten machine-command entry
// points emits commandRejected with a non-empty reason (no silent return).
void OperatorLifecycleDeveloperTest::duplicateRejectionForEveryEntryPoint()
{
    // Reset: the first reset enters homing, the duplicate is rejected.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);
        homeReady(gw);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->reset().accepted);
        QVERIFY(!c->reset().accepted);
        verifyRejected(rejected, Command::Reset);
    }
    // Adjust width: the first request is in flight.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);
        homeReady(gw);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->adjustWidth(300).accepted);
        QVERIFY(!c->adjustWidth(350).accepted);
        verifyRejected(rejected, Command::AdjustWidth);
    }
    // Mode switch: the M104 select write stays unconfirmed.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
            return acceptedResult();
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Admin);
        homeReady(gw);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->setMode(true).accepted);
        QVERIFY(!c->setMode(false).accepted);
        verifyRejected(rejected, Command::ModeSwitch);
    }
    // Start: the M101 pulse is accepted but never drives M3.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.startPulse = [](quint16) -> SubmissionResult {
            return acceptedResult(); // accepted but never applied
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Operator);
        homeReady(gw);
        putInAutoMode(gw);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->start().accepted);
        QVERIFY(!c->start().accepted);
        verifyRejected(rejected, Command::Start);
    }
    // Stop: the M102 pulse is accepted but never clears M3.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.startPulse = [](quint16) -> SubmissionResult {
            return acceptedResult(); // accepted but never applied
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Anonymous);
        homeReady(gw);
        putInAutoMode(gw);
        gw.model().writeCoil(kM101, true);
        gw.model().writeCoil(kM101, false);
        gw.tick();
        QVERIFY(gw.lastSnapshot().m3());
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->stop().accepted);
        QVERIFY(!c->stop().accepted);
        verifyRejected(rejected, Command::Stop);
    }
    // Estop set: pending until the M0/M100 readback confirms.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
            return acceptedResult();
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Anonymous);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->estopSet().accepted);
        QVERIFY(!c->estopSet().accepted);
        verifyRejected(rejected, Command::EstopSet);
    }
    // Estop release: pending until the release is confirmed.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->estopRelease().accepted);
        QVERIFY(!c->estopRelease().accepted);
        verifyRejected(rejected, Command::EstopRelease);
    }
    // Manual hold: the same address+value is already awaiting confirmation.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);
        homeReady(gw);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->manualHold(kM106, true).accepted);
        QVERIFY(!c->manualHold(kM106, true).accepted);
        verifyRejected(rejected, Command::ManualCommand);
    }
    // Manual latch: M109 set is already awaiting confirmation.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);
        homeReady(gw);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->manualLatch(kM109, true).accepted);
        QVERIFY(!c->manualLatch(kM109, true).accepted);
        verifyRejected(rejected, Command::ManualCommand);
    }
    // Bypass: M110 set is already awaiting confirmation.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);
        QVERIFY(c->bypass(kM110, true).accepted);
        QVERIFY(!c->bypass(kM110, true).accepted);
        verifyRejected(rejected, Command::Bypass);
    }
}

// D4: no optimistic success for manual hold / bypass; the terminal success is
// only emitted once a confirmed snapshot shows the requested state.
void OperatorLifecycleDeveloperTest::manualAndBypassConfirmOnlyFromSnapshot()
{
    // Manual hold press accepted by the transport but not applied.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.writeHold = [](quint16, bool) -> SubmissionResult {
            return acceptedResult();
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Admin);
        homeReady(gw);

        QVector<bool> results;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&](Command cmd, bool ok, const QString &) {
                    if (cmd == Command::ManualCommand)
                        results.append(ok);
                });

        QVERIFY(c->manualHold(kM106, true).accepted);
        QVERIFY2(results.isEmpty(), "manual hold reported a result before confirmation");
        gw.tick(); // snapshot still shows M106=0
        QVERIFY(results.isEmpty());

        gw.model().writeCoil(kM106, true);
        gw.tick(); // confirmed
        QCOMPARE(results.size(), 1);
        QVERIFY(results[0]);
        gw.tick(); // exactly one terminal result
        QCOMPARE(results.size(), 1);
    }
    // Bypass set accepted by the transport but not applied.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
            return acceptedResult();
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Admin);

        QVector<bool> results;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&](Command cmd, bool ok, const QString &) {
                    if (cmd == Command::Bypass)
                        results.append(ok);
                });

        QVERIFY(c->bypass(kM110, true).accepted);
        QVERIFY2(results.isEmpty(), "bypass reported a result before confirmation");
        gw.tick();
        QVERIFY(results.isEmpty());

        gw.model().writeCoil(kM110, true);
        gw.tick();
        QCOMPARE(results.size(), 1);
        QVERIFY(results[0]);
    }
}

// D4: an unconfirmed hold command converges through the injected-clock
// defensive timeout with a non-empty reason; never a second terminal result.
void OperatorLifecycleDeveloperTest::manualConfirmTimeoutConvergesViaInjectedClock()
{
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    ControlCoordinator::PulseTransport t = gatewayTransport(gw);
    t.writeHold = [](quint16, bool) -> SubmissionResult {
            return acceptedResult();
        };
    std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
    c->setRole(Role::Admin);
    homeReady(gw);

    QVector<QString> details;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::ManualCommand) {
                    QVERIFY(!ok);
                    details.append(detail);
                }
            });

    QVERIFY(c->manualHold(kM106, true).accepted);
    gw.tick();
    QVERIFY(details.isEmpty()); // still waiting for confirmation

    now += 3'001; // past the 3000 ms defensive timeout
    gw.tick();
    QCOMPARE(details.size(), 1);
    QVERIFY(!details[0].isEmpty());
    QVERIFY(details[0].contains(QStringLiteral("超时")));

    now += 60'000;
    gw.tick();
    QCOMPARE(details.size(), 1); // exactly one terminal state
}

// D4: a transport write rejection is visible (rejection or terminal failure),
// never silence and never success.
void OperatorLifecycleDeveloperTest::transportRejectionIsVisibleWithoutSuccess()
{
    // Manual hold.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.writeHold = [](quint16, bool) -> SubmissionResult {
            return rejectedResult(QStringLiteral("transport rejected the hold write"));
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Admin);
        homeReady(gw);

        QVector<bool> results;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&](Command cmd, bool ok, const QString &) {
                    if (cmd == Command::ManualCommand)
                        results.append(ok);
                });
        QSignalSpy rejected(c.get(), &ControlCoordinator::commandRejected);

        const ControlCoordinator::CommandResult r = c->manualHold(kM106, true);
        QVERIFY(!r.accepted);
        QVERIFY(!r.reason.isEmpty());
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!rejected[0][1].toString().isEmpty());
        QVERIFY(results.isEmpty()); // no optimistic success
    }
    // Bypass.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        ControlCoordinator::PulseTransport t = gatewayTransport(gw);
        t.writeCoil = [](quint16, bool, CommandPriority) -> SubmissionResult {
            return rejectedResult(QStringLiteral("transport rejected the write"));
        };
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, t));
        c->setRole(Role::Admin);

        QVector<bool> results;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&](Command cmd, bool ok, const QString &) {
                    if (cmd == Command::Bypass)
                        results.append(ok);
                });

        const ControlCoordinator::CommandResult r = c->bypass(kM110, true);
        QVERIFY(!r.accepted);
        QVERIFY(!r.reason.isEmpty());
        QVERIFY(results.isEmpty());
    }
}

// D5: release wording distinguishes the software request from the physical
// estop; a full release keeps the existing wording.
void OperatorLifecycleDeveloperTest::estopReleaseNamesPhysicalEstopWhenM0StillHeld()
{
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);

        QVERIFY(c->estopSet().accepted);
        gw.tick();
        QVERIFY(gw.lastSnapshot().m0());
        gw.model().setEstopReleaseStuck(true);

        bool reported = false;
        bool ok = false;
        QString detail;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&](Command cmd, bool resultOk, const QString &resultDetail) {
                    if (cmd == Command::EstopRelease) {
                        reported = true;
                        ok = resultOk;
                        detail = resultDetail;
                    }
                });

        QVERIFY(c->estopRelease().accepted);
        gw.tick();
        QVERIFY(!gw.lastSnapshot().m100()); // software request released
        QVERIFY(gw.lastSnapshot().m0());    // physical estop still held
        QVERIFY(reported);
        QVERIFY(ok);
        QVERIFY(detail.contains(QStringLiteral("实体急停")));
        QVERIFY(!detail.contains(QStringLiteral("急停已解除")));
    }
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(wire(gw, now, gatewayTransport(gw)));
        c->setRole(Role::Admin);

        QVERIFY(c->estopSet().accepted);
        gw.tick();
        QVERIFY(gw.lastSnapshot().m0());

        QString detail;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&](Command cmd, bool, const QString &resultDetail) {
                    if (cmd == Command::EstopRelease)
                        detail = resultDetail;
                });

        QVERIFY(c->estopRelease().accepted);
        gw.tick();
        QVERIFY(!gw.lastSnapshot().m0());
        QCOMPARE(detail, QStringLiteral("急停已解除"));
    }
}

void OperatorLifecycleDeveloperTest::lifecycleHelpersClassifyStates()
{
    QVERIFY(!isTerminal(OperatorCommandState::Idle));
    QVERIFY(!isTerminal(OperatorCommandState::Rejected));
    QVERIFY(!isTerminal(OperatorCommandState::Accepted));
    QVERIFY(!isTerminal(OperatorCommandState::Pending));
    QVERIFY(isTerminal(OperatorCommandState::Succeeded));
    QVERIFY(isTerminal(OperatorCommandState::Failed));
    QVERIFY(isTerminal(OperatorCommandState::TimedOut));
    QVERIFY(isTerminal(OperatorCommandState::CommunicationsLost));
    QVERIFY(isTerminal(OperatorCommandState::GatewayReplaced));

    const QVector<OperatorCommandState> states{
        OperatorCommandState::Idle,      OperatorCommandState::Rejected,
        OperatorCommandState::Accepted,  OperatorCommandState::Pending,
        OperatorCommandState::Succeeded, OperatorCommandState::Failed,
        OperatorCommandState::TimedOut,  OperatorCommandState::CommunicationsLost,
        OperatorCommandState::GatewayReplaced,
    };
    QSet<QString> rendered;
    for (OperatorCommandState state : states) {
        const QString text = toString(state);
        QVERIFY(!text.isEmpty());
        rendered.insert(text);
    }
    QCOMPARE(rendered.size(), states.size());
}

void OperatorLifecycleDeveloperTest::shellModelStoresProjectsAndClearsStatus()
{
    ShellModel model;
    QVector<OperatorCommandStatus> changes;
    connect(&model, &ShellModel::operatorCommandStatusChanged, this,
            [&changes](const OperatorCommandStatus &s) { changes.append(s); });

    QVERIFY(model.operatorCommandStatus().lifecycle_state == OperatorCommandState::Idle);

    OperatorCommandStatus status;
    status.command = Command::ManualCommand;
    status.lifecycle_state = OperatorCommandState::Pending;
    status.human_readable_detail = QStringLiteral("等待确认");
    status.command_generation = 7;
    model.setOperatorCommandStatus(status);

    QCOMPARE(changes.size(), 1);
    const OperatorCommandStatus stored = model.operatorCommandStatus();
    QVERIFY(stored.command == Command::ManualCommand);
    QVERIFY(stored.lifecycle_state == OperatorCommandState::Pending);
    QCOMPARE(stored.human_readable_detail, QStringLiteral("等待确认"));
    QCOMPARE(stored.command_generation, quint64(7));
    // Forward compatibility with PLC-HMI-003 correlation.
    QVERIFY(!stored.request_id.has_value());
    QCOMPARE(stored.gateway_generation, quint64(0));

    model.clearOperatorCommandStatus();
    QCOMPARE(changes.size(), 2);
    QVERIFY(model.operatorCommandStatus().lifecycle_state == OperatorCommandState::Idle);
    QVERIFY(model.operatorCommandStatus().human_readable_detail.isEmpty());
}

void OperatorLifecycleDeveloperTest::actionBarRendersProjectedStatus()
{
    ShellModel model;
    ActionBar bar(model);
    QLabel *label = bar.commandStatusLabel();
    QVERIFY(label != nullptr);

    // Idle: default text, no stale success/failure.
    QVERIFY(!label->text().contains(QStringLiteral("成功")));
    QVERIFY(!label->text().contains(QStringLiteral("失败")));

    OperatorCommandStatus pending;
    pending.command = Command::Reset;
    pending.lifecycle_state = OperatorCommandState::Pending;
    pending.human_readable_detail = QStringLiteral("等待 PLC 确认");
    model.setOperatorCommandStatus(pending);
    QVERIFY2(label->text().contains(QStringLiteral("等待 PLC 确认")),
             qPrintable(label->text()));
    QVERIFY(label->text().contains(QStringLiteral("等待确认")));

    OperatorCommandStatus terminal = pending;
    terminal.lifecycle_state = OperatorCommandState::Succeeded;
    terminal.human_readable_detail = QStringLiteral("复位完成");
    model.setOperatorCommandStatus(terminal);
    QVERIFY2(label->text().contains(QStringLiteral("复位完成")),
             qPrintable(label->text()));

    model.clearOperatorCommandStatus();
    QVERIFY2(!label->text().contains(QStringLiteral("复位完成")),
             "cleared status must not keep stale terminal text");
}

QTEST_MAIN(OperatorLifecycleDeveloperTest)
#include "operator_lifecycle_developer_test.moc"
