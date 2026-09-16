// Task 6 integration tests: SimulatedPlcGateway (spec §14.2, §15.4), revised
// for the PLC-HMI-003 correlated submission port.
// Deterministic: the model clock is advanced explicitly via the gateway's
// tick(); no real waits, no serial ports.
//
// Coverage:
// - 复位 → 调宽 → 自动 → 启动 → 停止 full flow.
// - 调宽条件失败、动态超时、M45、D110=10、急停和故障锁存.
// - 通讯中断、D140 冻结、非法值和恢复重连.
// - Revised port: submissions return request identity, exactly one correlated
//   completion per accepted submission, offline rejection without replay.

#include <QtTest>

#include <algorithm>

#include "adapters/simulator/simulated_plc_gateway.h"

using namespace hlm;

namespace {

struct CompletionRecord
{
    quint16 address = 0;
    bool result = false;
    QString error;
};

// Submit through the revised port and require acceptance. The in-process link
// applies the request to the model immediately; the terminal completion (with
// its readback confirmation) arrives on the next tick.
void requireCoil(SimulatedPlcGateway &gw, quint16 address, bool value)
{
    const SubmissionResult r = gw.submitWriteCoil(address, value);
    QVERIFY2(r.accepted, qPrintable(r.immediate_rejection_reason));
}

void requireRegister(SimulatedPlcGateway &gw, quint16 address, quint16 value)
{
    const SubmissionResult r = gw.submitWriteRegister(address, value);
    QVERIFY2(r.accepted, qPrintable(r.immediate_rejection_reason));
}

void requirePulse(SimulatedPlcGateway &gw, quint16 address)
{
    const SubmissionResult r = gw.submitPulse(address);
    QVERIFY2(r.accepted, qPrintable(r.immediate_rejection_reason));
}

} // namespace

class SimulatedGatewayFlowTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void startPublishesSnapshotAndGoesOnline();
    void fullFlowResetAdjustAutoStartStop();
    void adjustPreconditionFailure();
    void dynamicTimeoutFault10();
    void estopLatchesFault();
    void linkDownFreezesD140AndRejectsWrites();
    void linkDownRecoveryResumes();
    void heartbeatFreezeGoesOfflineAndRecovers();
    void freezeUnfreezeRefreezeResetsCounter();
    void everyAcceptedSubmissionCompletesExactlyOnce();
    void writeThenReadbackConfirmed();
    void offlineRejectsWrites();
    void unconfirmedWriteReportsFailure();
    void illegalValueMarksFieldInvalid();
    void snapshotIsAtomicAndComplete();
    void stopEmitsOffline();
    void restartAfterStopWorks();
    void tickAdvancesModelDeterministically();
    void startPulseWritesCoilPair();
    void commStatsEmitted();

private:
    SimulatedPlcGateway *m_gw = nullptr;
    int m_snapshots = 0;
    bool m_online = false;
    quint64 m_lastGeneration = 0;
    QList<CompletionRecord> m_completions;
    QList<quint64> m_statSequences;
    QList<int> m_statReconnects;
    QList<int> m_statFailedPolls;
};

void SimulatedGatewayFlowTest::init()
{
    m_gw = new SimulatedPlcGateway();
    m_snapshots = 0;
    m_online = false;
    m_lastGeneration = 0;
    m_completions.clear();
    m_statSequences.clear();
    m_statReconnects.clear();
    m_statFailedPolls.clear();
    connect(m_gw, &SimulatedPlcGateway::snapshotReady, this,
            [this](quint64 generation, const DeviceSnapshot &) {
                ++m_snapshots;
                m_lastGeneration = generation;
            });
    connect(m_gw, &SimulatedPlcGateway::connectionStateChanged, this,
            [this](quint64, bool online) { m_online = online; });
    connect(m_gw, &SimulatedPlcGateway::submissionCompleted, this,
            [this](const SubmissionCompletion &completion) {
                m_completions.append(
                    {completion.address, completion.result, completion.error});
            });
    connect(m_gw, &SimulatedPlcGateway::commStatsChanged, this,
            [this](const PlcCommStats &stats) {
                m_statSequences.append(stats.snapshot_sequence);
                m_statReconnects.append(int(stats.reconnect_count));
                m_statFailedPolls.append(int(stats.failed_polls));
            });
}

void SimulatedGatewayFlowTest::cleanup()
{
    delete m_gw;
    m_gw = nullptr;
}

void SimulatedGatewayFlowTest::startPublishesSnapshotAndGoesOnline()
{
    m_gw->start();
    QVERIFY(m_gw->isOnline());
    QVERIFY(m_online);
    QCOMPARE(m_snapshots, 1);
    QVERIFY(m_gw->hasSnapshot());
    QVERIFY(m_lastGeneration != 0);
    const DeviceSnapshot s = m_gw->lastSnapshot();
    QCOMPARE(s.sequence(), quint64(1));
    QVERIFY(s.connected());
    QCOMPARE(s.heartbeat(), quint16(0));
    QCOMPARE(s.currentWidth(), quint16(200));
    QCOMPARE(s.targetWidth(), quint16(200));
    QCOMPARE(s.beltSpeed(), quint16(1000));
    QCOMPARE(s.overallQuality(), DataQuality::Valid);
    QVERIFY(s.fast_quality == DataQuality::Valid);
    QVERIFY(s.overall_age_ms >= 0);
    QVERIFY(s.m1()); // manual mode default
    QVERIFY(!s.m2());
    QVERIFY(!s.m3());
}

void SimulatedGatewayFlowTest::fullFlowResetAdjustAutoStartStop()
{
    m_gw->start();

    // 复位 (M103 pulse) -> home return.
    requirePulse(*m_gw, 103);
    m_gw->tick();
    QVERIFY(m_completions.last().result);
    QCOMPARE(m_completions.last().address, quint16(103));
    QVERIFY(m_gw->lastSnapshot().m50()); // homing in progress
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m50()); // homed

    // 调宽: target 300, M43 pulse.
    requireRegister(*m_gw, 128, 300);
    requirePulse(*m_gw, 43);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m34()); // adjusting
    QVERIFY(!m_gw->lastSnapshot().m44());
    QVERIFY(!m_gw->lastSnapshot().m45());

    // ceil(100 / 15) = 7 s to complete.
    for (int i = 0; i < 7; ++i)
        m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m34());
    QVERIFY(m_gw->lastSnapshot().m44());
    QVERIFY(!m_gw->lastSnapshot().m45());
    QCOMPARE(m_gw->lastSnapshot().currentWidth(), quint16(300));

    // 自动模式 (M104).
    requireCoil(*m_gw, 104, true);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m2());
    QVERIFY(!m_gw->lastSnapshot().m1());

    // 启动 (M101 pulse).
    requirePulse(*m_gw, 101);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m3()); // running

    // 停止 (M102 pulse).
    requirePulse(*m_gw, 102);
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m3());
}

void SimulatedGatewayFlowTest::adjustPreconditionFailure()
{
    m_gw->start();
    // Not homed: M43 preconditions fail -> only M45 (spec §10.3.1).
    requireRegister(*m_gw, 128, 300);
    requirePulse(*m_gw, 43);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m45());
    QVERIFY(!m_gw->lastSnapshot().m44());
    QVERIFY(!m_gw->lastSnapshot().m34());
}

void SimulatedGatewayFlowTest::dynamicTimeoutFault10()
{
    m_gw->start();
    // Home return.
    requirePulse(*m_gw, 103);
    m_gw->tick();
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m50());

    // Stall the motor: positioning never completes -> dynamic timeout.
    m_gw->model().setPositioningStall(true);
    requireRegister(*m_gw, 128, 300);
    requirePulse(*m_gw, 43);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m34());

    // Timeout = ceil(100 / 15) + 5 = 12 s (spec §10.3.1).
    for (int i = 0; i < 12; ++i)
        m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m34());
    QVERIFY(!m_gw->lastSnapshot().m44());
    QVERIFY(m_gw->lastSnapshot().m45());
    QVERIFY(m_gw->lastSnapshot().m14()); // latched fault
    QCOMPARE(m_gw->lastSnapshot().faultCode(), quint16(10));
}

void SimulatedGatewayFlowTest::estopLatchesFault()
{
    m_gw->start();
    // Home return.
    requirePulse(*m_gw, 103);
    m_gw->tick();
    m_gw->tick();

    // Auto mode + start.
    requireCoil(*m_gw, 104, true);
    requirePulse(*m_gw, 101);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m3());

    // Software estop (M100): M0, latched fault M14, D110 = 1, running stops.
    requireCoil(*m_gw, 100, true);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m0());
    QVERIFY(m_gw->lastSnapshot().m14());
    QCOMPARE(m_gw->lastSnapshot().faultCode(), quint16(1));
    QVERIFY(!m_gw->lastSnapshot().m3());

    // Release clears M0 only; the fault stays latched until reset.
    requireCoil(*m_gw, 100, false);
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m0());
    QVERIFY(m_gw->lastSnapshot().m14());
    QCOMPARE(m_gw->lastSnapshot().faultCode(), quint16(1));

    // Reset (M103) clears the latched fault.
    requirePulse(*m_gw, 103);
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m14());
    QCOMPARE(m_gw->lastSnapshot().faultCode(), quint16(0));
}

void SimulatedGatewayFlowTest::linkDownFreezesD140AndRejectsWrites()
{
    m_gw->start();
    m_gw->tick();
    const quint16 hb = m_gw->lastSnapshot().heartbeat();
    QVERIFY(hb > 0);

    m_gw->setLinkDown(true);
    QVERIFY(!m_gw->isOnline());
    QVERIFY(!m_online);

    // Ticks are ignored while the link is down: D140 frozen.
    m_gw->tick();
    m_gw->tick();
    QCOMPARE(m_gw->lastSnapshot().heartbeat(), hb);

    // Submissions rejected while offline, not applied, not replayed, and no
    // completion is emitted (rejected submissions never complete).
    const int completionsBefore = m_completions.size();
    const SubmissionResult rejected = m_gw->submitWriteCoil(100, true);
    QVERIFY(!rejected.accepted);
    QVERIFY(!rejected.immediate_rejection_reason.isEmpty());
    QCOMPARE(m_completions.size(), completionsBefore);
    QVERIFY(!m_gw->model().readCoil(100));
}

void SimulatedGatewayFlowTest::linkDownRecoveryResumes()
{
    m_gw->start();
    m_gw->tick();
    const quint16 hbBefore = m_gw->lastSnapshot().heartbeat();
    const quint64 generationBefore = m_lastGeneration;

    m_gw->setLinkDown(true);
    QVERIFY(!m_gw->isOnline());
    m_gw->tick(); // ignored while down
    QCOMPARE(m_gw->lastSnapshot().heartbeat(), hbBefore);

    // Restore: still offline until the next tick reconnects with a fresh
    // snapshot (mirrors the real gateway's reconnect-then-snapshot rule).
    m_gw->setLinkDown(false);
    QVERIFY(!m_gw->isOnline());
    m_gw->tick();
    QVERIFY(m_gw->isOnline());
    QVERIFY(m_online);
    QVERIFY(m_gw->lastSnapshot().heartbeat() != hbBefore); // D140 moving again
    QVERIFY2(m_lastGeneration > generationBefore,
             "a reconnect must advance the gateway generation");

    // Submissions accepted again.
    requireCoil(*m_gw, 100, true);
    m_gw->tick();
    QVERIFY(m_completions.last().result);
    QVERIFY(m_gw->model().readCoil(100));
}

void SimulatedGatewayFlowTest::heartbeatFreezeGoesOfflineAndRecovers()
{
    m_gw->start();
    m_gw->tick();
    QVERIFY(m_gw->isOnline());

    // Freeze the heartbeat: the model stops advancing, D140 unchanged.
    m_gw->setHeartbeatFrozen(true);
    const quint16 hb = m_gw->lastSnapshot().heartbeat();
    m_gw->tick();
    m_gw->tick();
    QCOMPARE(m_gw->lastSnapshot().heartbeat(), hb); // frozen
    QVERIFY(m_gw->isOnline()); // not yet past the threshold

    // 3rd tick with an unchanged heartbeat -> offline (spec §8.4).
    m_gw->tick();
    QVERIFY(!m_gw->isOnline());
    QVERIFY(!m_online);

    // Submissions rejected while offline.
    QVERIFY(!m_gw->submitWriteCoil(100, true).accepted);

    // Unfreeze: the next tick reconnects with a fresh snapshot.
    m_gw->setHeartbeatFrozen(false);
    m_gw->tick();
    QVERIFY(m_gw->isOnline());
    QVERIFY(m_online);
    QVERIFY(m_gw->lastSnapshot().heartbeat() != hb); // D140 moving again
}

void SimulatedGatewayFlowTest::freezeUnfreezeRefreezeResetsCounter()
{
    m_gw->start();
    m_gw->tick();
    QVERIFY(m_gw->isOnline());

    // Freeze for 2 ticks: still online (threshold is 3).
    m_gw->setHeartbeatFrozen(true);
    m_gw->tick();
    m_gw->tick();
    QVERIFY(m_gw->isOnline());

    // Unfreeze before the threshold: the stale counter must be reset.
    m_gw->setHeartbeatFrozen(false);
    m_gw->tick();
    QVERIFY(m_gw->isOnline());

    // Re-freeze: must get a full 3 ticks before going offline, not 1.
    m_gw->setHeartbeatFrozen(true);
    m_gw->tick();
    m_gw->tick();
    QVERIFY(m_gw->isOnline());
    m_gw->tick();
    QVERIFY(!m_gw->isOnline());
    QVERIFY(!m_online);
}

void SimulatedGatewayFlowTest::everyAcceptedSubmissionCompletesExactlyOnce()
{
    m_gw->start();

    const SubmissionResult coil = m_gw->submitWriteCoil(100, true);
    const SubmissionResult reg = m_gw->submitWriteRegister(128, 300);
    const SubmissionResult pulse = m_gw->submitPulse(103);
    QVERIFY(coil.accepted);
    QVERIFY(reg.accepted);
    QVERIFY(pulse.accepted);
    QVERIFY(coil.request_id != 0 && reg.request_id != 0 && pulse.request_id != 0);
    QVERIFY(coil.request_id != reg.request_id);
    QVERIFY(coil.request_id != pulse.request_id);
    QVERIFY(reg.request_id != pulse.request_id);

    m_gw->tick();
    QCOMPARE(m_completions.size(), 3);
    for (const CompletionRecord &c : m_completions)
        QVERIFY(c.result);
    QCOMPARE(m_completions.at(0).address, quint16(100));
    QCOMPARE(m_completions.at(1).address, quint16(128));
    QCOMPARE(m_completions.at(2).address, quint16(103));

    // A further tick must not repeat any completion.
    m_gw->tick();
    QCOMPARE(m_completions.size(), 3);
}

void SimulatedGatewayFlowTest::writeThenReadbackConfirmed()
{
    m_gw->start();
    requireCoil(*m_gw, 100, true);
    QVERIFY(m_gw->model().readCoil(100));
    m_gw->tick();
    QCOMPARE(m_completions.size(), 1);
    QVERIFY(m_completions.last().result);
    QVERIFY(m_completions.last().error.isEmpty());

    requireRegister(*m_gw, 128, 300);
    QCOMPARE(m_gw->model().readRegister(128), quint16(300));
    m_gw->tick();
    QCOMPARE(m_completions.size(), 2);
    QVERIFY(m_completions.last().result);
}

void SimulatedGatewayFlowTest::offlineRejectsWrites()
{
    m_gw->start();
    m_gw->setLinkDown(true);
    QVERIFY(!m_gw->isOnline());

    const SubmissionResult coil = m_gw->submitWriteCoil(100, true);
    const SubmissionResult reg = m_gw->submitWriteRegister(128, 300);
    QVERIFY(!coil.accepted);
    QVERIFY(!reg.accepted);
    QVERIFY(!coil.immediate_rejection_reason.isEmpty());
    QVERIFY(!reg.immediate_rejection_reason.isEmpty());
    // Not applied, not replayed, no completion.
    QVERIFY(!m_gw->model().readCoil(100));
    QCOMPARE(m_gw->model().readRegister(128), quint16(200));
    QVERIFY(m_completions.isEmpty());
}

void SimulatedGatewayFlowTest::unconfirmedWriteReportsFailure()
{
    m_gw->start();
    // Address 200 is outside the model's coil range (M0-M112): the model
    // ignores the write, so the readback cannot confirm it. This is the
    // simulated analog of a request that times out without taking effect.
    const SubmissionResult r = m_gw->submitWriteCoil(200, true);
    QVERIFY(r.accepted);
    m_gw->tick();
    QCOMPARE(m_completions.size(), 1);
    QVERIFY(!m_completions.last().result);
    QVERIFY(!m_completions.last().error.isEmpty());
    QVERIFY(!m_gw->model().readCoil(200));
}

void SimulatedGatewayFlowTest::illegalValueMarksFieldInvalid()
{
    m_gw->start();
    // D128 outside 50-400: stored (the HMI validates before sending), but
    // the snapshot must mark the field invalid (spec §9).
    requireRegister(*m_gw, 128, 500);
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().fieldValid(SnapshotField::TargetWidth));
    QVERIFY(m_gw->lastSnapshot().fieldValid(SnapshotField::CurrentWidth));
    QCOMPARE(m_gw->lastSnapshot().targetWidth(), quint16(500));
}

void SimulatedGatewayFlowTest::snapshotIsAtomicAndComplete()
{
    m_gw->start();
    // Home return.
    requirePulse(*m_gw, 103);
    m_gw->tick();
    m_gw->tick();
    // Width adjust in progress.
    requireRegister(*m_gw, 128, 300);
    requirePulse(*m_gw, 43);
    m_gw->tick();

    const DeviceSnapshot s = m_gw->lastSnapshot();
    QVERIFY(s.connected());
    QVERIFY(s.sequence() > 0);
    // Fast block: status words and decoded fields.
    QVERIFY(s.m1()); // manual mode
    QVERIFY(!s.m2());
    QVERIFY(!s.m3());
    QVERIFY(!s.m0());
    QVERIFY(!s.m14());
    QVERIFY(s.m34()); // adjusting
    QVERIFY(!s.m44());
    QVERIFY(!s.m45());
    QCOMPARE(s.faultCode(), quint16(0));
    QCOMPARE(s.currentStep(), quint16(0));
    QCOMPARE(s.targetWidth(), quint16(300));
    QCOMPARE(s.currentWidth(), quint16(200));
    QCOMPARE(s.widthFrequency(), quint32(15 * 1280));
    QCOMPARE(s.pulsePerMm(), quint16(1280));
    QCOMPARE(s.widthSpeed(), quint16(15));
    QCOMPARE(s.widthDelta(), qint16(100));
    // Per-block quality/age and the independent D210 validity (D6).
    QVERIFY(s.fast_quality == DataQuality::Valid);
    QVERIFY(s.home_quality == DataQuality::Valid);
    QVERIFY(s.command_quality == DataQuality::Valid);
    QVERIFY(s.slow_quality == DataQuality::Valid);
    QVERIFY(s.overall_age_ms ==
            std::max({s.fast_age_ms, s.home_age_ms, s.command_age_ms, s.slow_age_ms}));
    QVERIFY(s.width_delta_valid);
    // Home bits: homing done.
    QVERIFY(!s.m50());
    // Command readback: all pulses low, M112 never exposed.
    QVERIFY(!s.m100());
    QVERIFY(!s.m101());
    QVERIFY(!s.m102());
    QVERIFY(!s.m103());
    QVERIFY(!s.m43());
    // §9: the simulator publishes an in-range default belt speed so the
    // complete snapshot is usable by the interactive --sim mode.
    QCOMPARE(s.beltSpeed(), quint16(1000));
    QVERIFY(s.fieldValid(SnapshotField::BeltSpeed));
    QVERIFY(s.fieldValid(SnapshotField::TargetWidth));
    QVERIFY(s.fieldValid(SnapshotField::CurrentWidth));
    QVERIFY(s.fieldValid(SnapshotField::FaultCode));
}

void SimulatedGatewayFlowTest::stopEmitsOffline()
{
    m_gw->start();
    QVERIFY(m_gw->isOnline());
    QVERIFY(m_online);

    m_gw->stop();
    QVERIFY(!m_gw->isOnline());
    QVERIFY(!m_online);

    // Submissions rejected after stop.
    QVERIFY(!m_gw->submitWriteCoil(100, true).accepted);
    QVERIFY(m_completions.isEmpty());
}

void SimulatedGatewayFlowTest::restartAfterStopWorks()
{
    m_gw->start();
    m_gw->tick();
    const int snapshotsBefore = m_snapshots;

    m_gw->stop();
    m_gw->start();
    QVERIFY(m_gw->isOnline());
    QVERIFY(m_online);
    QCOMPARE(m_snapshots, snapshotsBefore + 1); // fresh snapshot on restart
    QCOMPARE(m_gw->lastSnapshot().sequence(), quint64(1)); // sequence reset
}

void SimulatedGatewayFlowTest::tickAdvancesModelDeterministically()
{
    m_gw->start();
    QCOMPARE(m_gw->elapsedSeconds(), quint64(0));
    QCOMPARE(m_gw->lastSnapshot().heartbeat(), quint16(0));

    m_gw->tick();
    QCOMPARE(m_gw->elapsedSeconds(), quint64(1));
    QCOMPARE(m_gw->lastSnapshot().heartbeat(), quint16(1));

    // Tick cadence is configurable.
    m_gw->setTickSeconds(2);
    m_gw->tick();
    QCOMPARE(m_gw->elapsedSeconds(), quint64(3));
    QCOMPARE(m_gw->lastSnapshot().heartbeat(), quint16(3));
}

void SimulatedGatewayFlowTest::startPulseWritesCoilPair()
{
    m_gw->start();
    QVERIFY(m_gw->isOnline());

    // submitPulse(M103): the simulated gateway writes 1 then 0 and confirms.
    requirePulse(*m_gw, 103);
    m_gw->tick();
    QCOMPARE(m_completions.size(), 1);
    QVERIFY(m_completions.last().result);
    // The coil pair delivered the pulse: the bit is back to 0.
    QVERIFY(!m_gw->model().readCoil(103));

    // The rising edge triggered the reset/home-return flow (spec §10.2).
    QVERIFY(m_gw->lastSnapshot().m50()); // homing in progress
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m50()); // homed

    // Offline: submitPulse rejected, nothing written, no completion.
    m_gw->setLinkDown(true);
    QVERIFY(!m_gw->isOnline());
    const int completionsBefore = m_completions.size();
    QVERIFY(!m_gw->submitPulse(103).accepted);
    QCOMPARE(m_completions.size(), completionsBefore);
    QVERIFY(!m_gw->model().readCoil(103));
}

void SimulatedGatewayFlowTest::commStatsEmitted()
{
    m_gw->start(); // snapshot #1
    QCOMPARE(m_statSequences.size(), 1);
    QCOMPARE(m_statSequences.first(), quint64(1));
    QCOMPARE(m_statReconnects.first(), 0);
    QCOMPARE(m_statFailedPolls.first(), 0);

    // Link down/up: reconnectCount increments on restore; the next snapshot
    // carries the updated counters.
    m_gw->setLinkDown(true);
    m_gw->setLinkDown(false);
    m_gw->tick(); // reconnect snapshot
    QCOMPARE(m_statSequences.size(), 2);
    QCOMPARE(m_statReconnects.last(), 1);
    QCOMPARE(m_statFailedPolls.last(), 0); // in-process model never drops polls
}

QTEST_GUILESS_MAIN(SimulatedGatewayFlowTest)
#include "test_simulated_gateway_flow.moc"
