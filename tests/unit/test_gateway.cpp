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

private:
    FakeTransport *m_transport = nullptr;
    ModbusGatewayWorker *m_worker = nullptr;
    qint64 m_now = 0;
    int m_snapshots = 0;
    bool m_online = false;
    QList<QPair<quint16, bool>> m_writeResults;
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

    m_now = 0;
    m_snapshots = 0;
    m_online = false;
    m_writeResults.clear();

    connect(m_worker, &ModbusGatewayWorker::snapshotReady, this,
            [this](const DeviceSnapshot &) { ++m_snapshots; });
    connect(m_worker, &ModbusGatewayWorker::connectionStateChanged, this,
            [this](bool online) { m_online = online; });
    connect(m_worker, &ModbusGatewayWorker::writeCompleted, this,
            [this](quint16 addr, bool ok, const QString &) {
                m_writeResults.append({addr, ok});
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

QTEST_GUILESS_MAIN(GatewayTest)
#include "test_gateway.moc"
