// Task 6 integration tests: SimulatedPlcGateway (spec §14.2, §15.4).
// Deterministic: the model clock is advanced explicitly via the gateway's
// tick(); no real waits, no serial ports.
//
// Coverage required by the task brief:
// - 复位 → 调宽 → 自动 → 启动 → 停止 full flow.
// - 调宽条件失败、动态超时、M45、D110=10、急停和故障锁存.
// - 通讯中断、D140 冻结、请求超时、非法值和恢复重连.
// - Same IPlcGateway interface as the real gateway: write-then-readback
//   confirmation, offline rejection without replay, isOnline after a full
//   snapshot.

#include <QtTest>

#include "adapters/simulator/simulated_plc_gateway.h"

using namespace hlm;

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
    void writeThenReadbackConfirmed();
    void offlineRejectsWrites();
    void unconfirmedWriteReportsFailure();
    void illegalValueMarksFieldInvalid();
    void snapshotIsAtomicAndComplete();
    void stopEmitsOffline();
    void restartAfterStopWorks();
    void tickAdvancesModelDeterministically();

private:
    SimulatedPlcGateway *m_gw = nullptr;
    int m_snapshots = 0;
    bool m_online = false;
    QList<QPair<quint16, bool>> m_writeResults;
};

void SimulatedGatewayFlowTest::init()
{
    m_gw = new SimulatedPlcGateway();
    m_snapshots = 0;
    m_online = false;
    m_writeResults.clear();
    connect(m_gw, &SimulatedPlcGateway::snapshotReady, this,
            [this](const DeviceSnapshot &) { ++m_snapshots; });
    connect(m_gw, &SimulatedPlcGateway::connectionStateChanged, this,
            [this](bool online) { m_online = online; });
    connect(m_gw, &SimulatedPlcGateway::writeCompleted, this,
            [this](quint16 addr, bool ok, const QString &) {
                m_writeResults.append({addr, ok});
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
    const DeviceSnapshot s = m_gw->lastSnapshot();
    QCOMPARE(s.sequence(), quint64(1));
    QVERIFY(s.connected());
    QCOMPARE(s.heartbeat(), quint16(0));
    QCOMPARE(s.currentWidth(), quint16(200));
    QCOMPARE(s.targetWidth(), quint16(200));
    QVERIFY(s.m1()); // manual mode default
    QVERIFY(!s.m2());
    QVERIFY(!s.m3());
}

void SimulatedGatewayFlowTest::fullFlowResetAdjustAutoStartStop()
{
    m_gw->start();

    // 复位 (M103 pulse) -> home return.
    m_gw->writeCoil(103, true);
    QVERIFY(m_writeResults.last().second);
    m_gw->writeCoil(103, false);
    QVERIFY(m_writeResults.last().second);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m50()); // homing in progress
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m50()); // homed

    // 调宽: target 300, M43 pulse.
    m_gw->writeRegister(128, 300);
    QVERIFY(m_writeResults.last().second);
    m_gw->writeCoil(43, true);
    QVERIFY(m_writeResults.last().second);
    m_gw->writeCoil(43, false);
    QVERIFY(m_writeResults.last().second);
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
    m_gw->writeCoil(104, true);
    QVERIFY(m_writeResults.last().second);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m2());
    QVERIFY(!m_gw->lastSnapshot().m1());

    // 启动 (M101 pulse).
    m_gw->writeCoil(101, true);
    QVERIFY(m_writeResults.last().second);
    m_gw->writeCoil(101, false);
    QVERIFY(m_writeResults.last().second);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m3()); // running

    // 停止 (M102 pulse).
    m_gw->writeCoil(102, true);
    QVERIFY(m_writeResults.last().second);
    m_gw->writeCoil(102, false);
    QVERIFY(m_writeResults.last().second);
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m3());
}

void SimulatedGatewayFlowTest::adjustPreconditionFailure()
{
    m_gw->start();
    // Not homed: M43 preconditions fail -> only M45 (spec §10.3.1).
    m_gw->writeRegister(128, 300);
    m_gw->writeCoil(43, true);
    m_gw->writeCoil(43, false);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m45());
    QVERIFY(!m_gw->lastSnapshot().m44());
    QVERIFY(!m_gw->lastSnapshot().m34());
}

void SimulatedGatewayFlowTest::dynamicTimeoutFault10()
{
    m_gw->start();
    // Home return.
    m_gw->writeCoil(103, true);
    m_gw->writeCoil(103, false);
    m_gw->tick();
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m50());

    // Stall the motor: positioning never completes -> dynamic timeout.
    m_gw->model().setPositioningStall(true);
    m_gw->writeRegister(128, 300);
    m_gw->writeCoil(43, true);
    m_gw->writeCoil(43, false);
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
    m_gw->writeCoil(103, true);
    m_gw->writeCoil(103, false);
    m_gw->tick();
    m_gw->tick();

    // Auto mode + start.
    m_gw->writeCoil(104, true);
    m_gw->writeCoil(101, true);
    m_gw->writeCoil(101, false);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m3());

    // Software estop (M100): M0, latched fault M14, D110 = 1, running stops.
    m_gw->writeCoil(100, true);
    m_gw->tick();
    QVERIFY(m_gw->lastSnapshot().m0());
    QVERIFY(m_gw->lastSnapshot().m14());
    QCOMPARE(m_gw->lastSnapshot().faultCode(), quint16(1));
    QVERIFY(!m_gw->lastSnapshot().m3());

    // Release clears M0 only; the fault stays latched until reset.
    m_gw->writeCoil(100, false);
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().m0());
    QVERIFY(m_gw->lastSnapshot().m14());
    QCOMPARE(m_gw->lastSnapshot().faultCode(), quint16(1));

    // Reset (M103) clears the latched fault.
    m_gw->writeCoil(103, true);
    m_gw->writeCoil(103, false);
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

    // Writes rejected while offline, not applied, not replayed.
    m_gw->writeCoil(100, true);
    QCOMPARE(m_writeResults.size(), 1);
    QVERIFY(!m_writeResults.last().second);
    QVERIFY(!m_gw->model().readCoil(100));
}

void SimulatedGatewayFlowTest::linkDownRecoveryResumes()
{
    m_gw->start();
    m_gw->tick();
    const quint16 hbBefore = m_gw->lastSnapshot().heartbeat();

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

    // Writes accepted again.
    m_gw->writeCoil(100, true);
    QVERIFY(m_writeResults.last().second);
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

    // Writes rejected while offline.
    m_gw->writeCoil(100, true);
    QCOMPARE(m_writeResults.size(), 1);
    QVERIFY(!m_writeResults.last().second);

    // Unfreeze: the next tick reconnects with a fresh snapshot.
    m_gw->setHeartbeatFrozen(false);
    m_gw->tick();
    QVERIFY(m_gw->isOnline());
    QVERIFY(m_online);
    QVERIFY(m_gw->lastSnapshot().heartbeat() != hb); // D140 moving again
}

void SimulatedGatewayFlowTest::writeThenReadbackConfirmed()
{
    m_gw->start();
    m_gw->writeCoil(100, true);
    QCOMPARE(m_writeResults.size(), 1);
    QVERIFY(m_writeResults.last().second);
    QVERIFY(m_gw->model().readCoil(100));

    m_gw->writeRegister(128, 300);
    QCOMPARE(m_writeResults.size(), 2);
    QVERIFY(m_writeResults.last().second);
    QCOMPARE(m_gw->model().readRegister(128), quint16(300));
}

void SimulatedGatewayFlowTest::offlineRejectsWrites()
{
    m_gw->start();
    m_gw->setLinkDown(true);
    QVERIFY(!m_gw->isOnline());

    m_gw->writeCoil(100, true);
    m_gw->writeRegister(128, 300);
    QCOMPARE(m_writeResults.size(), 2);
    QVERIFY(!m_writeResults.at(0).second);
    QVERIFY(!m_writeResults.at(1).second);
    // Not applied, not replayed.
    QVERIFY(!m_gw->model().readCoil(100));
    QCOMPARE(m_gw->model().readRegister(128), quint16(200));
}

void SimulatedGatewayFlowTest::unconfirmedWriteReportsFailure()
{
    m_gw->start();
    // Address 200 is outside the model's coil range (M0-M112): the model
    // ignores the write, so the readback cannot confirm it. This is the
    // simulated analog of a request that times out without taking effect.
    m_gw->writeCoil(200, true);
    QCOMPARE(m_writeResults.size(), 1);
    QVERIFY(!m_writeResults.last().second);
    QVERIFY(!m_gw->model().readCoil(200));
}

void SimulatedGatewayFlowTest::illegalValueMarksFieldInvalid()
{
    m_gw->start();
    // D128 outside 50-400: stored (the HMI validates before sending), but
    // the snapshot must mark the field invalid (spec §9).
    m_gw->writeRegister(128, 500);
    QVERIFY(m_writeResults.last().second);
    m_gw->tick();
    QVERIFY(!m_gw->lastSnapshot().fieldValid(SnapshotField::TargetWidth));
    QVERIFY(m_gw->lastSnapshot().fieldValid(SnapshotField::CurrentWidth));
    QCOMPARE(m_gw->lastSnapshot().targetWidth(), quint16(500));
}

void SimulatedGatewayFlowTest::snapshotIsAtomicAndComplete()
{
    m_gw->start();
    // Home return.
    m_gw->writeCoil(103, true);
    m_gw->writeCoil(103, false);
    m_gw->tick();
    m_gw->tick();
    // Width adjust in progress.
    m_gw->writeRegister(128, 300);
    m_gw->writeCoil(43, true);
    m_gw->writeCoil(43, false);
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
    // Home bits: homing done.
    QVERIFY(!s.m50());
    // Command readback: all pulses low.
    QVERIFY(!s.m100());
    QVERIFY(!s.m101());
    QVERIFY(!s.m102());
    QVERIFY(!s.m103());
    QVERIFY(!s.m43());
    // §9: belt speed is not modeled -> out of range -> invalid; the fields
    // the model does simulate are valid.
    QVERIFY(!s.fieldValid(SnapshotField::BeltSpeed));
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

    // Writes rejected after stop.
    m_gw->writeCoil(100, true);
    QCOMPARE(m_writeResults.size(), 1);
    QVERIFY(!m_writeResults.last().second);
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

QTEST_GUILESS_MAIN(SimulatedGatewayFlowTest)
#include "test_simulated_gateway_flow.moc"
