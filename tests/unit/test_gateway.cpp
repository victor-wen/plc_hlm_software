// Task 4 unit tests: ModbusGatewayWorker state machine (spec §8.3, §8.4).
// Deterministic: a fake transport and an injected clock; no real serial port.
//
// Coverage required by the task brief:
// - D140 unchanged for 3 s -> offline (even though serial data still returns).
// - Offline: commands arriving while offline are rejected, not queued for
//   later replay.
// - Write-then-readback: write acknowledged, readback confirms success; a
//   readback mismatch retries; persistent mismatch reports failure.
// - Read retry-once: a failed read is retried once before counting as a
//   transfer failure.
// - Reconnect: after reconnect the gateway must get a full valid snapshot
//   before control resumes (isOnline() false until first snapshot).

#include <QtTest>

#include <QList>

#include "adapters/modbus/modbus_transport.h"
#include "adapters/modbus/qt_modbus_plc_gateway.h"

using namespace hlm;

namespace {

// Fake transport: records sent requests and lets the test complete them
// synchronously with scripted results.
class FakeTransport : public IModbusTransport
{
    Q_OBJECT

public:
    bool open() override
    {
        m_open = true;
        return true;
    }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }

    bool send(const ModbusRequest &req) override
    {
        sent.append(req);
        return !m_sendFails;
    }

    // Complete the most recent request with a scripted result.
    void completeOk(const QList<quint16> &values)
    {
        TransferResult res;
        res.ok = true;
        res.values = values;
        emit transferFinished(res);
    }

    void completeFail()
    {
        TransferResult res;
        res.ok = false;
        res.error = QStringLiteral("timeout");
        emit transferFinished(res);
    }

    bool m_open = false;
    bool m_sendFails = false;
    QList<ModbusRequest> sent;
};

// Build a 41-register fast block with the given heartbeat value.
QList<quint16> fastBlock(quint16 heartbeat)
{
    QList<quint16> raw(41, 0);
    raw[40] = heartbeat;
    return raw;
}

} // namespace

class GatewayTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void d140FreezeGoesOffline();
    void offlineRejectsCommands();
    void writeThenReadbackConfirmed();
    void writeThenReadbackMismatchRetries();
    void readRetriesOnce();
    void reconnectRequiresFullSnapshotBeforeOnline();
    void snapshotMetatypeRegistered();
    void reconnectResetsFailureCounterAndBackoff();
    void failedWriteEmitsWriteCompleted();
    void failedReadbackEmitsWriteCompletedAndNoLeak();
    void pollAtPendingWriteAddressNotConsumedAsReadback();
    void duplicateWritesBothGetConfirmations();
    void sendFailureEmitsWriteCompletedFalse();
    void offlineClearEmitsWriteCompletedFalseForInFlightWrite();
    void readbackSendFailureEmitsWriteCompletedFalse();
    void offlineReportsPendingAndQueuedWrites();
    void stopReportsPendingAndQueuedWrites();
    void stopEmitsConnectionStateChangedFalse();
    void restartAfterStopWorks();
    void writeRejectedUntilFirstSnapshotAfterReconnect();
    void slowPollOutOfRangeMarksFieldsInvalid();
    // Task 20: pulse state machine + M112 watchdog wiring, comm stats.
    void startPulseRoutesThroughStateMachine();
    void pulseHoldThenClearAtMin100ms();
    void pulseClearPriorityLevel1();
    void pulseAbortsOnOffline();
    void uncertainPulseSetConvergesViaReadback();
    void heartbeatFlipsM112Every500ms();
    void heartbeatStopsOffline();
    void commStatsEmitted();

private:
    FakeTransport *m_transport = nullptr;
    ModbusGatewayWorker *m_worker = nullptr;
    qint64 m_now = 0;
    int m_snapshots = 0;
    bool m_online = false;
    QList<QPair<quint16, bool>> m_writeResults;
    DeviceSnapshot m_lastSnapshot{DeviceSnapshotData()};
    // comm stats recording (Task 20).
    QList<quint64> m_statSequences;
    QList<int> m_statReconnects;
    QList<int> m_statFailedPolls;
};

void GatewayTest::init()
{
    m_transport = new FakeTransport();
    QtModbusPlcGateway::Config cfg;
    cfg.portName = QStringLiteral("fake");
    m_worker = new ModbusGatewayWorker(cfg, m_transport);
    m_worker->setNowMs([this]() { return m_now; });
    // Only the fast poll fires (home/command/slow effectively disabled) so
    // the tests are deterministic about which request is in flight.
    m_worker->setPollIntervals(250, 1000000, 1000000, 1000000);
    // The M112 watchdog would inject a flip into the request stream on the
    // first online tick; disable it so the existing tests keep their exact
    // dispatch-order assumptions. Heartbeat tests re-enable it explicitly.
    m_worker->setWatchdogEnabled(false);

    m_now = 0;
    m_snapshots = 0;
    m_online = false;
    m_writeResults.clear();
    m_statSequences.clear();
    m_statReconnects.clear();
    m_statFailedPolls.clear();

    connect(m_worker, &ModbusGatewayWorker::snapshotReady, this,
            [this](const DeviceSnapshot &s) {
                ++m_snapshots;
                m_lastSnapshot = s;
            });
    connect(m_worker, &ModbusGatewayWorker::connectionStateChanged, this,
            [this](bool online) { m_online = online; });
    connect(m_worker, &ModbusGatewayWorker::writeCompleted, this,
            [this](quint16 addr, bool ok, const QString &) {
                m_writeResults.append({addr, ok});
            });
    connect(m_worker, &ModbusGatewayWorker::commStatsChanged, this,
            [this](quint64 seq, int reconnects, int failed) {
                m_statSequences.append(seq);
                m_statReconnects.append(reconnects);
                m_statFailedPolls.append(failed);
            });

    m_worker->start();
    // start() opens the link and dispatches the first poll immediately.
    QVERIFY(m_transport->m_open);
    QVERIFY(!m_transport->sent.isEmpty());
}

void GatewayTest::d140FreezeGoesOffline()
{
    // First fast poll: heartbeat = 1.
    m_transport->completeOk(fastBlock(1));
    QCOMPARE(m_snapshots, 1);
    QVERIFY(m_worker->isOnline());

    // Advance 2.9 s with heartbeat unchanged: still online.
    m_now = 2900;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Advance past 3 s with heartbeat unchanged: offline, even though the
    // serial data still returns. (3200 - 2900 = 300 ms >= 250 ms poll
    // interval, so a fresh poll fires.)
    m_now = 3200;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_online);
}

void GatewayTest::offlineRejectsCommands()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Force offline via heartbeat freeze.
    m_now = 3200;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(!m_worker->isOnline());

    // A command arriving while offline must be rejected, not queued.
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().second, false); // rejected
    QCOMPARE(m_transport->sent.size(), 2);          // no new request sent
}

void GatewayTest::writeThenReadbackConfirmed()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    // Write dispatched.
    QCOMPARE(m_transport->sent.size(), 2);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);
    m_transport->completeOk({}); // write acknowledged

    // Readback dispatched (M100 -> coil bit 0 of the read block).
    QCOMPARE(m_transport->sent.size(), 3);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::ReadCoils);
    m_transport->completeOk({0x0001}); // M100 = 1

    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().second, true);
}

void GatewayTest::writeThenReadbackMismatchRetries()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    m_transport->completeOk({}); // write acknowledged
    m_transport->completeOk({0x0000}); // readback: M100 still 0

    // Not confirmed yet; a retry readback is queued (no failure emitted).
    QCOMPARE(m_writeResults.size(), 0);

    m_transport->completeOk({0x0001}); // now M100 = 1
    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().second, true);
}

void GatewayTest::readRetriesOnce()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Dispatch a fresh fast poll, then fail it. It must be retried once.
    m_now = 300;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.size(), 2);
    m_transport->completeFail();
    QCOMPARE(m_transport->sent.size(), 3); // retry dispatched
    m_transport->completeOk(fastBlock(2)); // retry succeeds
    QCOMPARE(m_snapshots, 2);
    QVERIFY(m_worker->isOnline());
}

void GatewayTest::reconnectRequiresFullSnapshotBeforeOnline()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Three consecutive transfer failures -> offline. Each read is retried
    // once, so a full failure cycle is: fail (retry dispatched) + fail.
    // A fresh poll must be dispatched before each cycle.
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #1
    m_now = 600;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #2
    m_now = 900;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #3 -> offline
    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_online);

    // Reconnect: openLink() clears the queue and dispatches a fresh poll.
    m_worker->onReconnectTick();
    QVERIFY(m_transport->m_open);
    QVERIFY(!m_worker->isOnline()); // not online until a full snapshot
    m_transport->completeOk(fastBlock(3));
    QVERIFY(m_worker->isOnline());
    QVERIFY(m_online);
}

void GatewayTest::snapshotMetatypeRegistered()
{
    // The facade registers the DeviceSnapshot metatype in its constructor so
    // queued worker->facade snapshot delivery works across threads.
    QtModbusPlcGateway::Config cfg;
    cfg.portName = QStringLiteral("fake");
    QtModbusPlcGateway facade(cfg);
    QVERIFY(QMetaType::fromType<hlm::DeviceSnapshot>().isRegistered());
}

void GatewayTest::reconnectResetsFailureCounterAndBackoff()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Three consecutive transfer failures -> offline (each read retried once).
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    m_now = 600;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    m_now = 900;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    QVERIFY(!m_worker->isOnline());

    // First reconnect attempt: backoff starts at 1 s.
    m_worker->onReconnectTick();
    QCOMPARE(m_worker->reconnectDelayMs(), 1000);
    m_transport->completeOk(fastBlock(2)); // full snapshot -> online again
    QVERIFY(m_worker->isOnline());

    // A single subsequent transfer failure must NOT take the link offline:
    // onReconnectSucceeded() reset the consecutive-failure counter.
    m_now = 1200;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #1 only
    QVERIFY(m_worker->isOnline());

    // And the backoff schedule restarted at 1 s for the next episode.
    m_now = 1500;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    m_now = 1800;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    QVERIFY(!m_worker->isOnline());
    m_worker->onReconnectTick();
    QCOMPARE(m_worker->reconnectDelayMs(), 1000);
}

void GatewayTest::failedWriteEmitsWriteCompleted()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    QCOMPARE(m_transport->sent.size(), 2);
    m_transport->completeFail(); // write transfer fails

    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().second, false);
}

void GatewayTest::failedReadbackEmitsWriteCompletedAndNoLeak()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    m_transport->completeOk({}); // write acknowledged
    QCOMPARE(m_transport->sent.size(), 3);
    m_transport->completeFail(); // readback transfer fails

    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().second, false);

    // The pending confirmation must not leak: a later poll at the same
    // address is a normal poll, not a readback.
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(2));
    QCOMPARE(m_snapshots, 2);
    QVERIFY(m_worker->isOnline());
}

void GatewayTest::pollAtPendingWriteAddressNotConsumedAsReadback()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Write 200 in flight, then enqueue a fast poll (D100, address 100) so
    // it sits in the queue. Four write cycles (200-203) dispatch while the
    // fast poll waits, pushing its skip counter to 4 (anti-starvation).
    m_worker->submitWriteCoil(200, true, CommandPriority::Normal);
    m_now = 300;
    m_worker->onPollTick(); // fast poll enqueued (write 200 in flight)
    m_transport->completeOk({}); // write 200 ack -> readback 200 dispatched
    m_worker->submitWriteCoil(201, true, CommandPriority::Normal);
    m_transport->completeOk({0x0001}); // readback 200 confirms
    m_transport->completeOk({}); // write 201 ack -> readback 201 dispatched
    m_worker->submitWriteCoil(202, true, CommandPriority::Normal);
    m_transport->completeOk({0x0001}); // readback 201 confirms
    m_transport->completeOk({}); // write 202 ack -> readback 202 dispatched
    m_worker->submitWriteCoil(203, true, CommandPriority::Normal);
    m_transport->completeOk({0x0001}); // readback 202 confirms
    m_transport->completeOk({}); // write 203 ack -> readback 203 dispatched
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    m_transport->completeOk({0x0001}); // readback 203 confirms
    m_transport->completeOk({}); // write 100 ack -> readback 100 enqueued

    // The fast poll (skip=4) is forced to the front and dispatches BEFORE
    // the M100 readback, even though both target address 100.
    QCOMPARE(m_transport->sent.last().cls, RequestClass::FastPoll);

    // The fast poll at address 100 must be handled as a poll, not consumed
    // as the readback: its data is delivered as a snapshot.
    m_transport->completeOk(fastBlock(2));
    QCOMPARE(m_snapshots, 2);

    // The M100 write is still pending; its own readback confirms it.
    QCOMPARE(m_writeResults.size(), 4);
    m_transport->completeOk({0x0001}); // readback 100 confirms
    QCOMPARE(m_writeResults.size(), 5);
    QCOMPARE(m_writeResults.last().second, true);
    QCOMPARE(m_writeResults.last().first, quint16(100));
}

void GatewayTest::duplicateWritesBothGetConfirmations()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Two writes to the same address; the second is queued behind the first
    // write + its readback. Dispatch order: write #1, write #2, readback #1,
    // readback #2 (both writes are level 4 and enqueued before the readbacks).
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    m_worker->submitWriteCoil(100, false, CommandPriority::Normal);
    m_transport->completeOk({}); // write #1 acknowledged
    m_transport->completeOk({}); // write #2 acknowledged
    m_transport->completeOk({0x0001}); // readback #1 confirms (M100 = 1)
    m_transport->completeOk({0x0000}); // readback #2 confirms (M100 = 0)

    QCOMPARE(m_writeResults.size(), 2);
    QCOMPARE(m_writeResults.at(0).second, true);
    QCOMPARE(m_writeResults.at(1).second, true);
}

void GatewayTest::sendFailureEmitsWriteCompletedFalse()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // send() returns false immediately (link dropped mid-flight). The write
    // must still report its result, not be silently dropped.
    m_transport->m_sendFails = true;
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);

    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().first, quint16(100));
    QCOMPARE(m_writeResults.first().second, false);

    // 1st/2nd such failure must NOT take the link offline (3-failure rule).
    QVERIFY(m_worker->isOnline());
    QVERIFY(m_online);

    m_worker->submitWriteCoil(101, true, CommandPriority::Normal);
    QCOMPARE(m_writeResults.size(), 2);
    QCOMPARE(m_writeResults.last().second, false);
    QVERIFY(m_worker->isOnline());

    // 3rd consecutive failure -> offline.
    m_worker->submitWriteCoil(102, true, CommandPriority::Normal);
    QCOMPARE(m_writeResults.size(), 3);
    QCOMPARE(m_writeResults.last().second, false);
    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_online);
}

void GatewayTest::offlineClearEmitsWriteCompletedFalseForInFlightWrite()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Put a write in flight, then drive the link offline via 3 consecutive
    // transfer failures. The in-flight write must be reported as failed.
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    QCOMPARE(m_transport->sent.size(), 2);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);

    // Three consecutive transfer failures -> offline. Each read is retried
    // once, so a full failure cycle is: fail (retry dispatched) + fail.
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #1
    m_now = 600;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #2
    m_now = 900;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #3 -> offline

    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_online);

    // The in-flight write was cleared by enterOffline() and reported once.
    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().first, quint16(100));
    QCOMPARE(m_writeResults.first().second, false);

    // A late transferFinished for the dropped write must not double-report.
    m_transport->completeOk({});
    QCOMPARE(m_writeResults.size(), 1);
}

void GatewayTest::readbackSendFailureEmitsWriteCompletedFalse()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // Write dispatched; the readback's send() will fail.
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    QCOMPARE(m_transport->sent.size(), 2);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);

    // Acknowledge the write; the readback is dispatched next and its send()
    // returns false. The write must still report its result and the pending
    // confirmation must be erased (no leak).
    m_transport->m_sendFails = true;
    m_transport->completeOk({}); // write acknowledged -> readback send fails
    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().first, quint16(100));
    QCOMPARE(m_writeResults.first().second, false);

    // 1st/2nd such failure must NOT take the link offline (3-failure rule).
    QVERIFY(m_worker->isOnline());
    QVERIFY(m_online);

    // No pending confirmation may leak: a later poll at the same address is
    // a normal poll, not a readback.
    m_transport->m_sendFails = false;
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(2));
    QCOMPARE(m_snapshots, 2);
    QVERIFY(m_worker->isOnline());
}

void GatewayTest::offlineReportsPendingAndQueuedWrites()
{
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // A write queued but not yet dispatched (a poll is in flight).
    m_now = 300;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.size(), 2);
    m_worker->submitWriteCoil(200, true, CommandPriority::Normal);
    QCOMPARE(m_transport->sent.size(), 2); // still only the poll in flight

    // Heartbeat freeze (D140 unchanged 3 s) takes the link offline while the
    // write is still queued. enterOffline() must report the queued write —
    // never silently dropped.
    m_now = 3200;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1)); // heartbeat still 1 -> freeze
    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_online);

    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.at(0).first, quint16(200));
    QCOMPARE(m_writeResults.at(0).second, false);
}

void GatewayTest::stopReportsPendingAndQueuedWrites()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    // Write #1 dispatched and acknowledged; its readback is pending.
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    QCOMPARE(m_transport->sent.size(), 2);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);
    m_transport->completeOk({}); // write #1 acknowledged -> readback pending

    // Write #2 submitted while the readback is in flight: stays queued.
    m_worker->submitWriteCoil(200, true, CommandPriority::Normal);
    QCOMPARE(m_transport->sent.size(), 3);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::ReadCoils);

    // stop() must report BOTH the pending-confirmation write and the queued
    // write as failed — never silently dropped.
    m_worker->stop();

    QCOMPARE(m_writeResults.size(), 2);
    QCOMPARE(m_writeResults.at(0).first, quint16(100));
    QCOMPARE(m_writeResults.at(0).second, false);
    QCOMPARE(m_writeResults.at(1).first, quint16(200));
    QCOMPARE(m_writeResults.at(1).second, false);
}

void GatewayTest::stopEmitsConnectionStateChangedFalse()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());
    QVERIFY(m_online);

    m_worker->stop();

    // The worker's stop() must not leave the online mirror stale.
    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_online);
}

void GatewayTest::restartAfterStopWorks()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    m_worker->stop();
    QVERIFY(!m_worker->isOnline());

    // A stop->start restart must re-run: reopen the link, dispatch a fresh
    // poll, and publish a snapshot again.
    m_worker->start();
    QVERIFY(m_transport->m_open);
    QCOMPARE(m_transport->sent.size(), 2); // fresh fast poll dispatched
    m_transport->completeOk(fastBlock(2));
    QVERIFY(m_worker->isOnline());
    QCOMPARE(m_snapshots, 2);
}

void GatewayTest::writeRejectedUntilFirstSnapshotAfterReconnect()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    // Drive the link offline via 3 consecutive transfer failures (each read
    // retried once: fail + fail per cycle).
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    m_now = 600;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    m_now = 900;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    QVERIFY(!m_worker->isOnline());

    // Reconnect attempt: the link opens and a fresh fast poll is dispatched,
    // but the immediate fast poll FAILS (no full snapshot yet).
    m_worker->onReconnectTick();
    QVERIFY(m_transport->m_open);
    QVERIFY(!m_worker->isOnline());
    m_transport->completeFail(); // immediate fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> still no snapshot

    // A write submitted in the reconnect window must be rejected, not
    // dispatched (spec §8.4: no control before a full valid snapshot).
    const int sentBeforeWrite = m_transport->sent.size();
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().first, quint16(100));
    QCOMPARE(m_writeResults.first().second, false); // rejected
    QCOMPARE(m_transport->sent.size(), sentBeforeWrite); // no new request sent

    // A successful fast poll arrives: the first full snapshot reopens the
    // queue and control resumes. (The failed immediate poll re-entered
    // offline, so trigger the next reconnect attempt first.)
    m_worker->onReconnectTick();
    m_transport->completeOk(fastBlock(2));
    QVERIFY(m_worker->isOnline());
    QVERIFY(m_online);

    // A subsequent write IS accepted and dispatched.
    const int sentBeforeWrite2 = m_transport->sent.size();
    m_worker->submitWriteCoil(100, true, CommandPriority::Normal);
    QCOMPARE(m_transport->sent.size(), sentBeforeWrite2 + 1);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);
    m_transport->completeOk({}); // write acknowledged
    m_transport->completeOk({0x0001}); // readback confirms
    QCOMPARE(m_writeResults.size(), 2);
    QCOMPARE(m_writeResults.last().second, true);
}

// A slow poll carrying out-of-range D204/D220 must mark those fields invalid
// in the published snapshot (spec §9), so the UI shows "—" not a bogus value.
void GatewayTest::slowPollOutOfRangeMarksFieldsInvalid()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    // Enable the slow poll and dispatch one with D204=0 (below 1) and
    // D220=99 (above 15). At 300 ms both the fast and slow polls enqueue; the
    // fast poll dispatches first, so complete it before the slow poll.
    m_worker->setPollIntervals(250, 1000000, 1000000, 250);
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(2)); // in-flight fast poll
    QCOMPARE(m_transport->sent.last().cls, RequestClass::SlowPoll);
    QList<quint16> slow(20, 0);
    slow[0] = 0;   // D204
    slow[16] = 99; // D220
    m_transport->completeOk(slow);

    // The next fast poll publishes the snapshot with the invalid fields.
    m_now = 600;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(3));
    QVERIFY(m_snapshots >= 3);
    QVERIFY(!m_lastSnapshot.fieldValid(SnapshotField::PulsePerMm));
    QVERIFY(!m_lastSnapshot.fieldValid(SnapshotField::WidthSpeed));
    QVERIFY(m_lastSnapshot.overallQuality() == DataQuality::OutOfRange);

    // In-range slow values keep the fields valid.
    QCOMPARE(m_transport->sent.last().cls, RequestClass::SlowPoll);
    QList<quint16> okSlow(20, 0);
    okSlow[0] = 1280; // D204
    okSlow[16] = 15;  // D220
    m_transport->completeOk(okSlow);
    m_now = 900;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(4));
    QVERIFY(m_lastSnapshot.fieldValid(SnapshotField::PulsePerMm));
    QVERIFY(m_lastSnapshot.fieldValid(SnapshotField::WidthSpeed));
}

// ---------------------------------------------------------------------------
// Task 20: pulse state machine + M112 watchdog wiring, comm stats (spec §8.5,
// §8.6, §16).
// ---------------------------------------------------------------------------

void GatewayTest::startPulseRoutesThroughStateMachine()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    // startPulse(101) -> the state machine enqueues write-1 (user priority).
    QVERIFY(m_worker->startPulse(101));
    QCOMPARE(m_transport->sent.size(), 2);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);
    QCOMPARE(m_transport->sent.last().address, quint16(101));
    QCOMPARE(m_transport->sent.last().value, quint16(1));
    QCOMPARE(m_transport->sent.last().cls, RequestClass::UserWrite);
}

void GatewayTest::pulseHoldThenClearAtMin100ms()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    QVERIFY(m_worker->startPulse(101));
    m_transport->completeOk({}); // write-1 ack -> holding, timing starts at t=0
    m_transport->completeOk({0x0001}); // write-1 readback confirms (M101=1)

    // Before 100 ms the clear must NOT be enqueued.
    m_now = 50;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.size(), 3); // write-1 + its readback only

    // At >= 100 ms the clear is enqueued at PulseClear priority (level 1).
    m_now = 100;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.size(), 4);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);
    QCOMPARE(m_transport->sent.last().address, quint16(101));
    QCOMPARE(m_transport->sent.last().value, quint16(0));
    QCOMPARE(m_transport->sent.last().cls, RequestClass::PulseClear);

    // Clear ack + readback confirm -> pulse complete, reported via
    // writeCompleted(address, true).
    m_transport->completeOk({}); // clear ack
    m_transport->completeOk({0x0000}); // clear readback confirms (M101=0)
    QCOMPARE(m_writeResults.size(), 2);
    QCOMPARE(m_writeResults.last().first, quint16(101));
    QCOMPARE(m_writeResults.last().second, true);
}

void GatewayTest::pulseClearPriorityLevel1()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    QVERIFY(m_worker->startPulse(101));
    m_transport->completeOk({}); // write-1 ack -> readback dispatched

    // A user write queued behind the pulse must not preempt the clear: the
    // clear (level 1) dispatches before the user write (level 4).
    m_worker->submitWriteCoil(200, true, CommandPriority::Normal);
    m_now = 100;
    m_worker->onPollTick(); // clear enqueued (readback still in flight)
    m_transport->completeOk({0x0001}); // write-1 readback confirms
    QCOMPARE(m_transport->sent.size(), 4);
    QCOMPARE(m_transport->sent.last().cls, RequestClass::PulseClear);
    QCOMPARE(m_transport->sent.last().address, quint16(101));
}

void GatewayTest::pulseAbortsOnOffline()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    // Drive offline via heartbeat freeze.
    m_now = 3200;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(!m_worker->isOnline());

    // Offline startPulse: rejected, nothing queued.
    const int sentBefore = m_transport->sent.size();
    QVERIFY(!m_worker->startPulse(101));
    QCOMPARE(m_transport->sent.size(), sentBefore); // no new request
}

void GatewayTest::uncertainPulseSetConvergesViaReadback()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    QVERIFY(m_worker->startPulse(101));
    m_transport->completeFail(); // write-1 times out (uncertain)

    // The failed write reports its result; the machine must NOT re-send 1:
    // it requests a dedicated single-coil readback (isReadback, matched by
    // request identity, NOT the write confirmation table).
    QCOMPARE(m_writeResults.size(), 1);
    QCOMPARE(m_writeResults.first().second, false);
    QCOMPARE(m_transport->sent.size(), 3);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::ReadCoils);
    QCOMPARE(m_transport->sent.last().address, quint16(101));
    QVERIFY(m_transport->sent.last().isReadback);

    // Readback shows the bit is 0: the set never took effect, the pulse is
    // aborted (finished(false) path) — no clear write is queued.
    m_transport->completeOk({0x0000});
    QCOMPARE(m_transport->sent.size(), 3); // no re-send, no clear
    QCOMPARE(m_writeResults.size(), 1);    // only the failed write reported
}

void GatewayTest::heartbeatFlipsM112Every500ms()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());

    // Re-enable the watchdog: the next online tick flips M112 immediately.
    m_worker->setWatchdogEnabled(true);
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.size(), 2);
    QCOMPARE(m_transport->sent.last().kind, ModbusRequest::Kind::WriteCoil);
    QCOMPARE(m_transport->sent.last().address, quint16(112));
    QCOMPARE(m_transport->sent.last().value, quint16(1));
    QCOMPARE(m_transport->sent.last().cls, RequestClass::Heartbeat);
    m_transport->completeOk({}); // flip ack
    m_transport->completeOk({0x0001}); // flip readback confirms

    // 499 ms later: no flip yet (a fast poll fires at 250 ms; complete it).
    m_now = 499;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.last().cls, RequestClass::FastPoll);
    m_transport->completeOk(fastBlock(1));

    // 500 ms: flip to 0 (dispatches before the fast poll, level 3 < 5).
    m_now = 500;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.size(), 5);
    QCOMPARE(m_transport->sent.last().address, quint16(112));
    QCOMPARE(m_transport->sent.last().value, quint16(0));
    QCOMPARE(m_transport->sent.last().cls, RequestClass::Heartbeat);
}

void GatewayTest::heartbeatStopsOffline()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll
    QVERIFY(m_worker->isOnline());
    m_worker->setWatchdogEnabled(true);
    m_worker->onPollTick(); // flip #1 (M112=1)
    QCOMPARE(m_transport->sent.size(), 2);
    m_transport->completeOk({}); // flip ack
    m_transport->completeOk({0x0001}); // flip readback confirms

    // Drive offline via heartbeat freeze. At t=3200 the watchdog also flips
    // (M112=0, level 3) and dispatches before the fast poll (level 5);
    // complete that flip before the poll.
    m_now = 3200;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.last().cls, RequestClass::Heartbeat);
    m_transport->completeOk({}); // flip ack
    m_transport->completeOk({0x0000}); // flip readback confirms
    QCOMPARE(m_transport->sent.last().cls, RequestClass::FastPoll);
    m_transport->completeOk(fastBlock(1)); // heartbeat unchanged -> freeze
    QVERIFY(!m_worker->isOnline());

    // Offline: no further flips queued.
    const int sentAfterOffline = m_transport->sent.size();
    m_now = 4000;
    m_worker->onPollTick();
    QCOMPARE(m_transport->sent.size(), sentAfterOffline);
}

void GatewayTest::commStatsEmitted()
{
    m_transport->completeOk(fastBlock(1)); // first fast poll -> snapshot #1
    QCOMPARE(m_statSequences.size(), 1);
    QCOMPARE(m_statSequences.first(), quint64(1));
    QCOMPARE(m_statReconnects.first(), 0);
    QCOMPARE(m_statFailedPolls.first(), 0);

    // A failed poll (retry exhausted) increments failedPolls; the next
    // snapshot carries the updated counters.
    m_now = 300;
    m_worker->onPollTick();
    m_transport->completeFail(); // fast poll fails -> retry
    m_transport->completeFail(); // retry fails -> failure #1 (not offline)
    QVERIFY(m_worker->isOnline());
    m_now = 600;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(2)); // snapshot #2
    QCOMPARE(m_statSequences.size(), 2);
    QCOMPARE(m_statFailedPolls.last(), 1);

    // Drive offline (3 consecutive failures) and reconnect: reconnectCount
    // increments when the reconnect is scheduled (enterOffline).
    m_now = 900;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    m_now = 1200;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    m_now = 1500;
    m_worker->onPollTick();
    m_transport->completeFail();
    m_transport->completeFail();
    QVERIFY(!m_worker->isOnline());
    m_worker->onReconnectTick();
    m_transport->completeOk(fastBlock(3)); // snapshot #3
    QCOMPARE(m_statSequences.size(), 3);
    QCOMPARE(m_statReconnects.last(), 1);
    // 1 (earlier) + 3 offline-driving cycles = 4 failed polls.
    QCOMPARE(m_statFailedPolls.last(), 4);
}

QTEST_GUILESS_MAIN(GatewayTest)
#include "test_gateway.moc"
