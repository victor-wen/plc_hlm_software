// PLC-HMI-003 black-box tests: ControlCoordinator correlation of PLC
// submissions and completions by request identity + gateway generation
// (brief OB-2, OB-3, OB-4, OB-5, OB-11).
//
// The tests drive the coordinator through its revised seam only:
//   * ControlCoordinator::PulseTransport callbacks returning SubmissionResult,
//   * ControlCoordinator::onSubmissionCompleted(const SubmissionCompletion &).
// The PulseTransport test double below is a fake gateway-facing source: it
// answers every submission with a documented SubmissionResult, issues its own
// request ids per generation, and can deliver correlated terminal completions.
// A SimulatedPlcGateway only supplies snapshots/connection state, so no
// production request path is read or assumed.
//
// Authored from the behavior-only brief .ai/test-briefs/PLC-HMI-003.yaml and
// the approved .ai/project-contract.yaml. No production implementation source
// was read.
//
// The revised contract API does not exist on the current tree, so a compile
// failure of this target is the expected RED.

#include <QtTest>
#include <QSignalSpy>

#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "application/control_coordinator.h"

using namespace hlm;

namespace {

constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kM106 = 106;
constexpr quint16 kD128 = 128;

constexpr quint64 kAdjustTimeoutMs = 3'600'001;
constexpr quint64 kModeSwitchTimeoutMs = 60'000;
constexpr quint64 kResetTimeoutMs = 200'000;

struct SubmissionRecord
{
    quint64 request_id = 0;
    quint64 gateway_generation = 0;
    PlcOperation operation;
    quint16 address = 0;
    bool coilValue = false;
    quint16 registerValue = 0;
};

// Fake gateway-facing submission source. It stands in for any IPlcGateway
// implementation: every callback returns a structured SubmissionResult and the
// completion for an accepted request can be delivered with its exact
// request_id + gateway_generation identity.
class FakeSubmissionSource
{
public:
    ControlCoordinator::PulseTransport transport()
    {
        ControlCoordinator::PulseTransport t;
        t.startPulse = [this](quint16 address) -> SubmissionResult {
            return record(PlcOperation::Pulse, address, false, 0);
        };
        t.writeHold = [this](quint16 address, bool value) -> SubmissionResult {
            return record(PlcOperation::WriteCoil, address, value, 0);
        };
        t.writeCoil = [this](quint16 address, bool value, CommandPriority)
            -> SubmissionResult {
            return record(PlcOperation::WriteCoil, address, value, 0);
        };
        t.writeRegister = [this](quint16 address, quint16 value, CommandPriority)
            -> SubmissionResult {
            return record(PlcOperation::WriteRegister, address, false, value);
        };
        return t;
    }

    const QVector<SubmissionRecord> &records() const { return m_records; }

    bool rejectSubmissions = false;
    QString rejectionReason = QStringLiteral("fake gateway unavailable");
    quint64 generation = 1;

    void complete(ControlCoordinator &coordinator, quint64 requestId, bool result,
                  const QString &error = QString())
    {
        SubmissionCompletion completion;
        completion.request_id = requestId;
        completion.gateway_generation = generation;
        completion.operation = operationFor(requestId);
        completion.address = addressFor(requestId);
        completion.result = result;
        completion.error = error;
        coordinator.onSubmissionCompleted(completion);
    }

    void completeWithGeneration(ControlCoordinator &coordinator, quint64 requestId,
                                quint64 completionGeneration, bool result)
    {
        SubmissionCompletion completion;
        completion.request_id = requestId;
        completion.gateway_generation = completionGeneration;
        completion.operation = operationFor(requestId);
        completion.address = addressFor(requestId);
        completion.result = result;
        completion.error = QStringLiteral("wrong generation");
        coordinator.onSubmissionCompleted(completion);
    }

private:
    SubmissionResult record(PlcOperation operation, quint16 address, bool coilValue,
                            quint16 registerValue)
    {
        SubmissionResult r;
        if (rejectSubmissions) {
            r.accepted = false;
            r.request_id = 0;
            r.gateway_generation = generation;
            r.immediate_rejection_reason = rejectionReason;
            return r;
        }
        r.accepted = true;
        r.request_id = m_nextRequestId++;
        r.gateway_generation = generation;
        m_records.append(SubmissionRecord{r.request_id, generation, operation, address,
                                          coilValue, registerValue});
        return r;
    }

    PlcOperation operationFor(quint64 requestId) const
    {
        for (const SubmissionRecord &r : m_records)
            if (r.request_id == requestId)
                return r.operation;
        return PlcOperation::WriteCoil;
    }

    quint16 addressFor(quint64 requestId) const
    {
        for (const SubmissionRecord &r : m_records)
            if (r.request_id == requestId)
                return r.address;
        return 0;
    }

    QVector<SubmissionRecord> m_records;
    quint64 m_nextRequestId = 1;
};

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

struct Rig
{
    SimulatedPlcGateway gateway;
    qint64 now = 0;
    FakeSubmissionSource fake;
    std::unique_ptr<ControlCoordinator> coordinator;

    void start()
    {
        gateway.start();
        for (int i = 0; i < 10 && !gateway.isOnline(); ++i)
            gateway.tick();
        coordinator.reset(new ControlCoordinator(fake.transport(),
                                                 ControlCoordinator::Config(),
                                                 [this]() { return now; }));
        QObject::connect(&gateway, &SimulatedPlcGateway::snapshotReady,
                         coordinator.get(),
                         [this](quint64, const DeviceSnapshot &s) {
                             coordinator->onSnapshot(s);
                         });
        QObject::connect(&gateway, &SimulatedPlcGateway::connectionStateChanged,
                         coordinator.get(),
                         [this](quint64, bool online) {
                             coordinator->onConnectionChanged(online);
                         });
        if (gateway.hasSnapshot())
            coordinator->onSnapshot(gateway.lastSnapshot());
    }
};

} // namespace

class PlcCommandCorrelationTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-2: completion correlation by request_id + generation --------------
    void failedCompletionFailsExactlyItsCorrelatedCommand();
    void unknownRequestIdCompletionIsIgnored();
    void obsoleteGenerationCompletionIsIgnored();
    void duplicateCompletionProducesExactlyOneTerminal();
    void lateCompletionAfterTimeoutIsIgnored();
    void lateCompletionFromAnEarlierRequestCannotSatisfyANewerRequest();

    // --- OB-3: overlapping same-address commands and duplicates ---------------
    void overlappingSameAddressCommandsNeverConsumeEachOthersCompletion();
    void duplicateInProgressRequestIsVisiblyRejectedWithoutNewRequestId();

    // --- OB-4: replacement/offline convergence and stale events ---------------
    void offlineConvergesPendingWriteAndLateCompletionIsIgnored();

    // --- OB-5: parameter writes are correlated and bounded --------------------
    void parameterWriteTimeoutConvergesAndLaterWritesAreNotBlocked();
    void offlineRejectsNewSubmissionsWithAVisibleReason();
};

// --- OB-2 ---------------------------------------------------------------------

void PlcCommandCorrelationTest::failedCompletionFailsExactlyItsCorrelatedCommand()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVector<bool> outcomes;
    QString detail;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Reset) {
                    outcomes.append(ok);
                    detail = d;
                }
            });

    QVERIFY(rig.coordinator->reset().accepted);
    QVERIFY(rig.coordinator->resetInProgress());
    QVERIFY2(outcomes.isEmpty(), "an accepted command reported a result before its completion");
    QVERIFY(!rig.fake.records().isEmpty());

    const SubmissionRecord first = rig.fake.records().first();
    rig.fake.complete(*rig.coordinator, first.request_id, false,
                      QStringLiteral("modbus exception 0x02"));

    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);
    QVERIFY(!detail.isEmpty());
    QVERIFY(!rig.coordinator->resetInProgress());

    // Exactly one terminal result: later snapshots must not emit another.
    rig.gateway.tick();
    QCOMPARE(outcomes.size(), 1);
}

void PlcCommandCorrelationTest::unknownRequestIdCompletionIsIgnored()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVector<bool> outcomes;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Reset)
                    outcomes.append(ok);
            });

    QVERIFY(rig.coordinator->reset().accepted);
    const quint64 pendingId = rig.fake.records().first().request_id;

    rig.fake.complete(*rig.coordinator, pendingId + 987654, false,
                      QStringLiteral("unknown request"));
    QVERIFY2(outcomes.isEmpty(), "a completion with an unknown request id was consumed");
    QVERIFY(rig.coordinator->resetInProgress());

    rig.fake.complete(*rig.coordinator, pendingId, false, QStringLiteral("real completion"));
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);
}

void PlcCommandCorrelationTest::obsoleteGenerationCompletionIsIgnored()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVector<bool> outcomes;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Reset)
                    outcomes.append(ok);
            });

    QVERIFY(rig.coordinator->reset().accepted);
    const SubmissionRecord pending = rig.fake.records().first();

    // An event from an older/unknown generation must never complete or fail a
    // current command (contract: Application rejects obsolete generations).
    rig.fake.completeWithGeneration(*rig.coordinator, pending.request_id, 0, false);
    QVERIFY2(outcomes.isEmpty(), "a generation-0 completion was consumed");
    rig.fake.completeWithGeneration(*rig.coordinator, pending.request_id,
                                    pending.gateway_generation + 7, false);
    QVERIFY2(outcomes.isEmpty(), "a future-generation completion was consumed");
    QVERIFY(rig.coordinator->resetInProgress());

    rig.fake.complete(*rig.coordinator, pending.request_id, false,
                      QStringLiteral("matching generation"));
    QCOMPARE(outcomes.size(), 1);
}

void PlcCommandCorrelationTest::duplicateCompletionProducesExactlyOneTerminal()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVector<bool> outcomes;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Reset)
                    outcomes.append(ok);
            });

    QVERIFY(rig.coordinator->reset().accepted);
    const quint64 id = rig.fake.records().first().request_id;

    rig.fake.complete(*rig.coordinator, id, false, QStringLiteral("first"));
    rig.fake.complete(*rig.coordinator, id, false, QStringLiteral("duplicate"));
    rig.fake.complete(*rig.coordinator, id, true, QStringLiteral("duplicate success"));

    QCOMPARE(outcomes.size(), 1); // exactly one terminal outcome
    QVERIFY(!outcomes[0]);

    rig.gateway.tick();
    QCOMPARE(outcomes.size(), 1);
}

void PlcCommandCorrelationTest::lateCompletionAfterTimeoutIsIgnored()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVector<bool> outcomes;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Reset)
                    outcomes.append(ok);
            });

    QVERIFY(rig.coordinator->reset().accepted);
    const quint64 id = rig.fake.records().first().request_id;

    rig.now += static_cast<qint64>(kResetTimeoutMs);
    rig.gateway.tick();
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);
    QVERIFY(!rig.coordinator->resetInProgress());

    // A late success must not turn an already-terminal timeout into success,
    // and must not produce a second terminal result.
    rig.fake.complete(*rig.coordinator, id, true);
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);

    rig.now += static_cast<qint64>(kResetTimeoutMs);
    rig.gateway.tick();
    QCOMPARE(outcomes.size(), 1);
}

void PlcCommandCorrelationTest::lateCompletionFromAnEarlierRequestCannotSatisfyANewerRequest()
{
    // The same M104 address is written by two sequential commands. An old
    // completion must never be attributed to the newer request (contract:
    // address-only/positional correlation is forbidden).
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);

    QVector<bool> outcomes;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::ModeSwitch)
                    outcomes.append(ok);
            });

    QVERIFY(rig.coordinator->setMode(true).accepted);
    const quint64 firstId = rig.fake.records().first().request_id;

    rig.now += static_cast<qint64>(kModeSwitchTimeoutMs);
    rig.gateway.tick();
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);

    const ControlCoordinator::CommandResult second = rig.coordinator->setMode(false);
    QVERIFY2(second.accepted, "a later mode switch must not be blocked by the timed-out one");
    QCOMPARE(rig.fake.records().size(), 2);
    const quint64 secondId = rig.fake.records()[1].request_id;
    QVERIFY(secondId != firstId);
    QCOMPARE(outcomes.size(), 1); // the new command is still pending

    // A stale success for the first request must not satisfy the second.
    rig.fake.complete(*rig.coordinator, firstId, true);
    QCOMPARE(outcomes.size(), 1);

    // Only the second request's own completion may converge it.
    rig.fake.complete(*rig.coordinator, secondId, false,
                      QStringLiteral("M104 write failed"));
    QCOMPARE(outcomes.size(), 2);
    QVERIFY(!outcomes[1]);
}

// --- OB-3 ---------------------------------------------------------------------

void PlcCommandCorrelationTest::overlappingSameAddressCommandsNeverConsumeEachOthersCompletion()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVector<bool> resetOutcomes;
    QVector<bool> modeOutcomes;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::Reset)
                    resetOutcomes.append(ok);
                if (cmd == Command::ModeSwitch)
                    modeOutcomes.append(ok);
            });
    QSignalSpy rejected(rig.coordinator.get(), &ControlCoordinator::commandRejected);

    QVERIFY(rig.coordinator->reset().accepted); // writes M104 = 0
    QVERIFY(rig.coordinator->resetInProgress());

    const ControlCoordinator::CommandResult second = rig.coordinator->setMode(true);
    if (!second.accepted) {
        // The brief allows a visibly rejected overlap instead of an accepted
        // concurrent command; it must never be silent.
        QVERIFY(!second.reason.isEmpty());
        QCOMPARE(rejected.count(), 1);
        QVERIFY(!rejected[0][1].toString().isEmpty());
        return;
    }

    // Both commands are pending and each owns its own request identity.
    QVERIFY2(rig.fake.records().size() >= 2,
             "both overlapping M104 commands must own their submissions");
    const quint64 modeId = rig.fake.records().last().request_id;
    const quint64 resetId = rig.fake.records().first().request_id;
    QVERIFY(modeId != resetId);

    // Only the mode switch may consume the mode switch's outcome.
    rig.fake.complete(*rig.coordinator, modeId, false, QStringLiteral("M104 write failed"));
    QCOMPARE(modeOutcomes.size(), 1);
    QVERIFY(!modeOutcomes[0]);
    QVERIFY2(resetOutcomes.isEmpty(), "the reset consumed the mode switch completion");
    QVERIFY(rig.coordinator->resetInProgress());

    // The reset converges through its own identity only.
    rig.fake.complete(*rig.coordinator, resetId, false, QStringLiteral("M104 write failed"));
    QCOMPARE(resetOutcomes.size(), 1);
    QVERIFY(!resetOutcomes[0]);
}

void PlcCommandCorrelationTest::duplicateInProgressRequestIsVisiblyRejectedWithoutNewRequestId()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVERIFY(rig.coordinator->reset().accepted);
    const int submissionsBefore = rig.fake.records().size();

    QSignalSpy rejected(rig.coordinator.get(), &ControlCoordinator::commandRejected);
    const ControlCoordinator::CommandResult duplicate = rig.coordinator->reset();

    QVERIFY(!duplicate.accepted);
    QVERIFY(!duplicate.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QCOMPARE(rejected[0][0].value<Command>(), Command::Reset);
    QVERIFY(!rejected[0][1].toString().isEmpty());
    QCOMPARE(rig.fake.records().size(), submissionsBefore); // no new request id allocated
}

// --- OB-4 ---------------------------------------------------------------------

void PlcCommandCorrelationTest::offlineConvergesPendingWriteAndLateCompletionIsIgnored()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);
    putInAutoMode(rig.gateway);

    QVector<bool> outcomes;
    QString detail;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &d) {
                if (cmd == Command::Reset) {
                    outcomes.append(ok);
                    detail = d;
                }
            });

    QVERIFY(rig.coordinator->reset().accepted);
    const quint64 id = rig.fake.records().first().request_id;

    rig.coordinator->onConnectionChanged(false);

    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);
    QVERIFY(!detail.isEmpty());
    QVERIFY(!rig.coordinator->resetInProgress());

    // A completion for the old generation that arrives after convergence must
    // not produce a second terminal result.
    rig.fake.complete(*rig.coordinator, id, true);
    QCOMPARE(outcomes.size(), 1);
    rig.gateway.tick();
    QCOMPARE(outcomes.size(), 1);
}

// --- OB-5 ---------------------------------------------------------------------

void PlcCommandCorrelationTest::parameterWriteTimeoutConvergesAndLaterWritesAreNotBlocked()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);

    QVector<bool> outcomes;
    connect(rig.coordinator.get(), &ControlCoordinator::commandResult, this,
            [&](Command cmd, bool ok, const QString &) {
                if (cmd == Command::AdjustWidth)
                    outcomes.append(ok);
            });

    QVERIFY(rig.coordinator->adjustWidth(300).accepted);
    QVERIFY(rig.coordinator->adjustInProgress());
    QVERIFY2(outcomes.isEmpty(), "a parameter write reported success before confirmation");

    // The completion is lost: the defensive timeout converges it visibly.
    rig.now += static_cast<qint64>(kAdjustTimeoutMs);
    rig.gateway.tick();
    QCOMPARE(outcomes.size(), 1);
    QVERIFY(!outcomes[0]);
    QVERIFY(!rig.coordinator->adjustInProgress());

    // A later parameter write is not blocked by the lost completion.
    const ControlCoordinator::CommandResult second = rig.coordinator->adjustWidth(350);
    QVERIFY2(second.accepted,
             "a lost parameter completion blocked the next parameter write");
    const quint64 secondId = rig.fake.records().last().request_id;
    rig.fake.complete(*rig.coordinator, secondId, false,
                      QStringLiteral("second write failed"));
    QCOMPARE(outcomes.size(), 2);
    QVERIFY(!outcomes[1]);
}

void PlcCommandCorrelationTest::offlineRejectsNewSubmissionsWithAVisibleReason()
{
    Rig rig;
    rig.start();
    rig.coordinator->setRole(Role::Admin);
    homeReady(rig.gateway);

    QVERIFY(rig.coordinator->adjustWidth(300).accepted);
    QVERIFY(rig.coordinator->adjustInProgress());

    rig.coordinator->onConnectionChanged(false);
    QVERIFY(!rig.coordinator->adjustInProgress());

    const int submissionsBefore = rig.fake.records().size();
    QSignalSpy rejected(rig.coordinator.get(), &ControlCoordinator::commandRejected);

    const ControlCoordinator::CommandResult offline = rig.coordinator->adjustWidth(350);
    QVERIFY2(!offline.accepted, "an offline parameter write must be rejected");
    QVERIFY(!offline.reason.isEmpty());
    QCOMPARE(rejected.count(), 1);
    QVERIFY(!rejected[0][1].toString().isEmpty());
    QCOMPARE(rig.fake.records().size(), submissionsBefore); // nothing was submitted
}

QTEST_GUILESS_MAIN(PlcCommandCorrelationTest)
#include "plc_command_correlation_test.moc"
