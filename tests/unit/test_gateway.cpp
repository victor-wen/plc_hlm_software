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
        return true;
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

QTEST_GUILESS_MAIN(GatewayTest)
#include "test_gateway.moc"
