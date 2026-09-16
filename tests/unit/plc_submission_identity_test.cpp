// PLC-HMI-003 black-box tests: IPlcGateway submission identity, correlated
// completions, gateway generation and port-level communication statistics,
// exercised through the in-process SimulatedPlcGateway implementation of the
// revised port (brief OB-1, OB-2, OB-4, OB-6, OB-12).
//
// Authored from the behavior-only brief .ai/test-briefs/PLC-HMI-003.yaml and
// the approved .ai/project-contract.yaml. No production implementation source
// was read.
//
// The revised contract API (SubmissionResult, SubmissionCompletion,
// PlcOperation, PlcCommStats, generation-carrying signals, submitWriteCoil /
// submitWriteRegister / submitPulse) does not exist on the current tree, so a
// compile failure of this target is the expected RED.

#include <QtTest>

#include <QVector>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "ports/iplc_gateway.h"

using namespace hlm;

namespace {

constexpr quint16 kM103 = 103;
constexpr quint16 kM106 = 106;
constexpr quint16 kM112 = 112;
constexpr quint16 kD128 = 128;

constexpr quint64 kRejectedRequestId = 0;

struct CompletionRecord
{
    quint64 request_id = 0;
    quint64 gateway_generation = 0;
    PlcOperation operation;
    quint16 address = 0;
    bool result = false;
    QString error;
};

void advanceUntilOnline(SimulatedPlcGateway &gw, int maxTicks = 10)
{
    for (int i = 0; i < maxTicks && !gw.isOnline(); ++i)
        gw.tick();
}

void advanceUntilOffline(SimulatedPlcGateway &gw, int maxTicks = 10)
{
    gw.setLinkDown(true);
    for (int i = 0; i < maxTicks && gw.isOnline(); ++i)
        gw.tick();
}

template <typename P>
constexpr bool priorityNamesExist()
{
    return requires {
        P::Normal;
        P::Safety;
        P::PulseClear;
    };
}

template <typename P>
constexpr bool priorityHasHeartbeat()
{
    return requires { P::Heartbeat; };
}

} // namespace

class PlcSubmissionIdentityTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1: structured accepted/rejected submission results ---------------
    void acceptedSubmissionsCarryUniqueNonZeroRequestIdsWithinGeneration();
    void offlineSubmissionsAreRejectedWithoutRequestIdAndWithReason();
    void documentedPrioritiesExistAndNoHeartbeatPriorityExists();

    // --- OB-2: exactly one correlated completion per accepted request --------
    void everyAcceptedSubmissionEmitsExactlyOneCorrelatedCompletion();
    void rejectedSubmissionEmitsNoCompletion();

    // --- OB-4: generation identifies the live gateway ------------------------
    void reconnectAdvancesGatewayGenerationAndNewEventsCarryIt();
    void submissionsNeverTargetCoil112();

    // --- OB-6: communication statistics arrive through the port signal -------
    void communicationStatisticsArriveThroughThePortSignal();
};

// --- OB-1 ---------------------------------------------------------------------

void PlcSubmissionIdentityTest::acceptedSubmissionsCarryUniqueNonZeroRequestIdsWithinGeneration()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());

    const SubmissionResult coil = gw.submitWriteCoil(kM106, true);
    const SubmissionResult reg = gw.submitWriteRegister(kD128, 300);
    const SubmissionResult pulse = gw.submitPulse(kM103);

    QVERIFY2(coil.accepted, "an online coil write must be accepted");
    QVERIFY2(reg.accepted, "an online register write must be accepted");
    QVERIFY2(pulse.accepted, "an online pulse must be accepted");

    for (const SubmissionResult &r : {coil, reg, pulse}) {
        QVERIFY2(r.request_id != kRejectedRequestId,
                 "an accepted submission must carry a non-zero request id");
        QVERIFY2(r.gateway_generation != 0,
                 "an accepted submission must carry its gateway generation");
        QVERIFY2(r.immediate_rejection_reason.isEmpty(),
                 "an accepted submission must not carry a rejection reason");
    }

    QVERIFY2(coil.request_id != reg.request_id,
             "request ids must be unique within a generation");
    QVERIFY2(coil.request_id != pulse.request_id,
             "request ids must be unique within a generation");
    QVERIFY2(reg.request_id != pulse.request_id,
             "request ids must be unique within a generation");
    QCOMPARE(coil.gateway_generation, reg.gateway_generation);
    QCOMPARE(coil.gateway_generation, pulse.gateway_generation);

    // A fourth submission must not reuse an earlier id while its outcome can
    // still arrive (contract lifecycle rule).
    const SubmissionResult coil2 = gw.submitWriteCoil(kM106, false);
    QVERIFY(coil2.accepted);
    QVERIFY2(coil2.request_id != coil.request_id, "request id reuse");
    QVERIFY2(coil2.request_id != reg.request_id, "request id reuse");
    QVERIFY2(coil2.request_id != pulse.request_id, "request id reuse");
    QCOMPARE(coil2.gateway_generation, coil.gateway_generation);

    // Priorities: the documented Normal/Safety submissions are accepted.
    const SubmissionResult normal =
        gw.submitWriteCoil(kM106, true, CommandPriority::Normal);
    const SubmissionResult safety =
        gw.submitWriteCoil(kM106, false, CommandPriority::Safety);
    QVERIFY(normal.accepted);
    QVERIFY(safety.accepted);
}

void PlcSubmissionIdentityTest::offlineSubmissionsAreRejectedWithoutRequestIdAndWithReason()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());

    advanceUntilOffline(gw);
    QVERIFY2(!gw.isOnline(), "the simulated link did not reach the offline state");

    const SubmissionResult coil = gw.submitWriteCoil(kM106, true);
    QVERIFY2(!coil.accepted, "an offline submission must be rejected");
    QCOMPARE(coil.request_id, kRejectedRequestId);
    QVERIFY2(!coil.immediate_rejection_reason.isEmpty(),
             "an offline rejection must carry an immediate reason");

    const SubmissionResult reg = gw.submitWriteRegister(kD128, 300);
    QVERIFY2(!reg.accepted, "an offline register write must be rejected");
    QCOMPARE(reg.request_id, kRejectedRequestId);
    QVERIFY2(!reg.immediate_rejection_reason.isEmpty(),
             "an offline rejection must carry an immediate reason");

    const SubmissionResult pulse = gw.submitPulse(kM103);
    QVERIFY2(!pulse.accepted, "an offline pulse must be rejected");
    QCOMPARE(pulse.request_id, kRejectedRequestId);
    QVERIFY2(!pulse.immediate_rejection_reason.isEmpty(),
             "an offline rejection must carry an immediate reason");
}

void PlcSubmissionIdentityTest::documentedPrioritiesExistAndNoHeartbeatPriorityExists()
{
    // OB-1 / ARCH-002: Normal, Safety and PulseClear exist; no Heartbeat
    // priority may exist because M112 is removed from the live path.
    static_assert(priorityNamesExist<CommandPriority>(),
                  "CommandPriority must expose Normal, Safety and PulseClear");
    static_assert(!priorityHasHeartbeat<CommandPriority>(),
                  "no Heartbeat priority may exist (M112 is removed)");

    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QCOMPARE(gw.submitPulse(kM103).accepted, true);
}

// --- OB-2 ---------------------------------------------------------------------

void PlcSubmissionIdentityTest::everyAcceptedSubmissionEmitsExactlyOneCorrelatedCompletion()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);

    QVector<CompletionRecord> completions;
    connect(&gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&completions](const SubmissionCompletion &c) {
                completions.append(CompletionRecord{c.request_id, c.gateway_generation,
                                                    c.operation, c.address, c.result,
                                                    c.error});
            });

    struct Submitted
    {
        SubmissionResult result;
        PlcOperation operation;
        quint16 address;
    };
    const Submitted coil{gw.submitWriteCoil(kM106, true), PlcOperation::WriteCoil, kM106};
    const Submitted reg{gw.submitWriteRegister(kD128, 300), PlcOperation::WriteRegister,
                        kD128};
    const Submitted pulse{gw.submitPulse(kM103), PlcOperation::Pulse, kM103};
    const QVector<Submitted> submitted{coil, reg, pulse};

    for (int i = 0; i < 6; ++i)
        gw.tick();

    QCOMPARE(completions.size(), submitted.size());
    for (const Submitted &s : submitted) {
        QVERIFY(s.result.accepted);
        int matches = 0;
        for (const CompletionRecord &c : completions) {
            if (c.request_id != s.result.request_id)
                continue;
            ++matches;
            QCOMPARE(c.gateway_generation, s.result.gateway_generation);
            QVERIFY2(c.operation == s.operation,
                     "the completion must carry the submitted operation");
            QCOMPARE(c.address, s.address);
            QVERIFY2(c.result, "the simulated transfer must complete successfully");
            QVERIFY2(c.error.isEmpty(), "a successful completion must not carry an error");
        }
        QCOMPARE(matches, 1); // exactly one terminal completion
    }
}

void PlcSubmissionIdentityTest::rejectedSubmissionEmitsNoCompletion()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);

    QVector<CompletionRecord> completions;
    connect(&gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&completions](const SubmissionCompletion &c) {
                completions.append(CompletionRecord{c.request_id, c.gateway_generation,
                                                    c.operation, c.address, c.result,
                                                    c.error});
            });

    advanceUntilOffline(gw);
    QVERIFY(!gw.isOnline());

    const SubmissionResult rejected = gw.submitWriteCoil(kM106, true);
    QVERIFY(!rejected.accepted);

    for (int i = 0; i < 4; ++i)
        gw.tick();

    QVERIFY2(completions.isEmpty(),
             "a rejected submission must not emit a completion");
}

// --- OB-4 ---------------------------------------------------------------------

void PlcSubmissionIdentityTest::reconnectAdvancesGatewayGenerationAndNewEventsCarryIt()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);

    const SubmissionResult before = gw.submitWriteCoil(kM106, true);
    QVERIFY(before.accepted);
    const quint64 firstGeneration = before.gateway_generation;
    QVERIFY(firstGeneration != 0);

    advanceUntilOffline(gw);
    QVERIFY(!gw.isOnline());

    gw.setLinkDown(false);
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());
    for (int i = 0; i < 3; ++i)
        gw.tick();

    QVector<quint64> generations;
    connect(&gw, &SimulatedPlcGateway::snapshotReady, this,
            [&generations](quint64 generation, const DeviceSnapshot &) {
                generations.append(generation);
            });
    gw.tick();
    QVERIFY2(!generations.isEmpty(),
             "the reconnected gateway must publish snapshots");
    for (quint64 generation : generations) {
        QVERIFY2(generation > firstGeneration,
                 "post-reconnect snapshots must carry the new gateway generation");
    }

    const SubmissionResult after = gw.submitWriteCoil(kM106, false);
    QVERIFY(after.accepted);
    QVERIFY2(after.gateway_generation > firstGeneration,
             "a submission after replacement must carry the new gateway generation");
}

void PlcSubmissionIdentityTest::submissionsNeverTargetCoil112()
{
    // OB-7: during an online session no write to coil 112 is submitted.
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());

    QVector<quint16> submittedAddresses;
    connect(&gw, &SimulatedPlcGateway::submissionCompleted, this,
            [&submittedAddresses](const SubmissionCompletion &c) {
                submittedAddresses.append(c.address);
            });

    gw.submitWriteCoil(kM106, true);
    for (int i = 0; i < 20; ++i) {
        gw.tick();
        QVERIFY2(!gw.model().readCoil(kM112),
                 "coil 112 must never be energized by the HMI");
    }

    for (quint16 address : submittedAddresses)
        QVERIFY2(address != kM112, "the HMI submitted a write to coil 112");
}

// --- OB-6 ---------------------------------------------------------------------

void PlcSubmissionIdentityTest::communicationStatisticsArriveThroughThePortSignal()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);

    QVector<PlcCommStats> stats;
    connect(&gw, &SimulatedPlcGateway::commStatsChanged, this,
            [&stats](const PlcCommStats &s) { stats.append(s); });

    for (int i = 0; i < 5; ++i)
        gw.tick();

    QVERIFY2(!stats.isEmpty(),
             "the gateway port must publish communication statistics");

    for (const PlcCommStats &s : stats) {
        QVERIFY2(s.per_block_age_ms >= 0,
                 "communication statistics must carry a real, non-negative data age");
    }
    for (int i = 1; i < stats.size(); ++i) {
        QVERIFY(stats[i].snapshot_sequence >= stats[i - 1].snapshot_sequence);
        QVERIFY(stats[i].reconnect_count >= stats[i - 1].reconnect_count);
        QVERIFY(stats[i].failed_polls >= stats[i - 1].failed_polls);
        QCOMPARE(stats[i].gateway_generation, stats[0].gateway_generation);
    }
}

QTEST_GUILESS_MAIN(PlcSubmissionIdentityTest)
#include "plc_submission_identity_test.moc"
