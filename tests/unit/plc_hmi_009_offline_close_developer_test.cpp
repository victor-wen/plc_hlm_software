// PLC-HMI-009 R2 developer-owned D2 regression tests: a logical-offline
// transition must close the underlying transport (three consecutive transfer
// failures or a frozen heartbeat), an already-closed transport must be left
// alone, reconnect must reopen it, and a write rejected during the offline
// window must never be replayed.
//
// Rig convention mirrors tests/unit/plc_hmi_009_offline_close_test.cpp: the
// gateway worker is driven exclusively through its public test seams and an
// injected fake IModbusTransport. This file is developer-owned and does not
// modify the independent test.

#include <QtTest>

#include "adapters/modbus/modbus_transport.h"
#include "adapters/modbus/qt_modbus_plc_gateway.h"
#include "ports/iplc_gateway.h"

using namespace hlm;

namespace {

constexpr quint16 kM106 = 106;

// Fast poll block image: the fast block carries the D140 heartbeat in its last
// register (index 40 of 41); every other value is neutral.
QList<quint16> fastBlock(quint16 heartbeat)
{
    QList<quint16> raw;
    raw.fill(0, 41);
    raw[40] = heartbeat;
    return raw;
}

// Deterministic in-process transport: transfers complete only when the test
// says so; open/close calls and the reported open state are observable.
class FakeTransport : public IModbusTransport
{
    Q_OBJECT
public:
    bool open() override
    {
        ++openCalls;
        m_open = true;
        return true;
    }

    void close() override
    {
        ++closeCalls;
        m_open = false;
    }

    bool isOpen() const override { return m_open; }

    bool send(const ModbusRequest &request) override
    {
        sent.append(request);
        return !m_sendFails;
    }

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
        emit transferFinished(res);
    }

    bool m_open = false;
    bool m_sendFails = false;
    int openCalls = 0;
    int closeCalls = 0;
    QList<ModbusRequest> sent;
};

} // namespace

class PlcHmi009OfflineCloseDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void offlineAfterThreeFailuresClosesTheTransport();
    void offlineAfterFrozenHeartbeatClosesTheTransport();
    void closedTransportIsLeftAloneOnOfflineTransition();
    void reconnectReopensTheTransport();
    void rejectedWriteIsNotReplayedAfterReconnect();
    void cleanup();

private:
    void buildWorker()
    {
        m_now = 0;
        m_transport = new FakeTransport;
        QtModbusPlcGateway::Config cfg;
        cfg.portName = QStringLiteral("fake");
        m_worker = new ModbusGatewayWorker(cfg, m_transport);
        m_worker->setNowMs([this] { return m_now; });
        m_worker->setPollIntervals(250, 1000000, 1000000, 1000000);
        m_worker->start();
    }

    // Three consecutive poll cycles, each with the read and its retry failing.
    void driveToOffline()
    {
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
    }

    FakeTransport *m_transport = nullptr;
    ModbusGatewayWorker *m_worker = nullptr;
    qint64 m_now = 0;
};

void PlcHmi009OfflineCloseDeveloperTest::cleanup()
{
    delete m_worker;
    m_worker = nullptr;
    delete m_transport;
    m_transport = nullptr;
    m_now = 0;
}

void PlcHmi009OfflineCloseDeveloperTest::offlineAfterThreeFailuresClosesTheTransport()
{
    buildWorker();

    // A healthy first transfer puts the gateway online; start() opened the link.
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());
    QVERIFY(m_transport->m_open);

    driveToOffline();
    QVERIFY2(!m_worker->isOnline(),
             "three consecutive transfer failures take the gateway offline");
    QVERIFY2(!m_transport->m_open,
             "the 3-failure offline transition must close the transport");
    QCOMPARE(m_transport->closeCalls, 1);
}

void PlcHmi009OfflineCloseDeveloperTest::offlineAfterFrozenHeartbeatClosesTheTransport()
{
    buildWorker();

    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // The first unchanged D140 reading is still inside the approved window.
    m_now = 2900;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // The next unchanged reading freezes the heartbeat -> offline.
    m_now = 3200;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY2(!m_worker->isOnline(),
             "a frozen heartbeat takes the gateway offline");
    QVERIFY2(!m_transport->m_open,
             "the frozen-heartbeat offline transition must close the transport");
    QCOMPARE(m_transport->closeCalls, 1);
}

void PlcHmi009OfflineCloseDeveloperTest::closedTransportIsLeftAloneOnOfflineTransition()
{
    buildWorker();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    // The transport is no longer open (e.g. the port already dropped): the
    // offline transition must not issue another close on it.
    m_transport->close();
    const int closesBefore = m_transport->closeCalls;

    driveToOffline();
    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_transport->m_open);
    QCOMPARE(m_transport->closeCalls, closesBefore);
}

void PlcHmi009OfflineCloseDeveloperTest::reconnectReopensTheTransport()
{
    buildWorker();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    driveToOffline();
    QVERIFY(!m_worker->isOnline());
    QVERIFY(!m_transport->m_open);
    const int opensBefore = m_transport->openCalls;

    m_worker->onReconnectTick();
    QVERIFY2(m_transport->m_open, "reconnect must open the transport again");
    QCOMPARE(m_transport->openCalls, opensBefore + 1);

    // A valid snapshot after reconnect restores the online session.
    m_transport->completeOk(fastBlock(5));
    QVERIFY(m_worker->isOnline());
}

void PlcHmi009OfflineCloseDeveloperTest::rejectedWriteIsNotReplayedAfterReconnect()
{
    buildWorker();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    driveToOffline();
    QVERIFY(!m_worker->isOnline());

    // A command submitted while offline is rejected visibly and never reaches
    // the transport.
    const int sentBeforeRejectedWrite = int(m_transport->sent.size());
    const SubmissionResult rejected =
        m_worker->submitWriteCoil(kM106, true, CommandPriority::Normal);
    QVERIFY2(!rejected.accepted, "a command submitted while offline must be rejected");
    QVERIFY2(!rejected.immediate_rejection_reason.isEmpty(),
             "an offline rejection must carry a visible reason");
    QCOMPARE(int(m_transport->sent.size()), sentBeforeRejectedWrite);

    // Reconnect: only the poll that restores the session is dispatched; the
    // rejected write is not queued and never replayed.
    m_worker->onReconnectTick();
    m_transport->completeOk(fastBlock(7));
    QVERIFY(m_worker->isOnline());
    QCOMPARE(int(m_transport->sent.size()), sentBeforeRejectedWrite + 1);

    // A later poll is a plain read: no write for M106 appears.
    m_now = 1000;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(7));
    for (const ModbusRequest &r : m_transport->sent) {
        QVERIFY2(!(r.kind == ModbusRequest::Kind::WriteCoil && r.address == kM106),
                 "a write rejected while offline must never be replayed");
    }
}

QTEST_GUILESS_MAIN(PlcHmi009OfflineCloseDeveloperTest)
#include "plc_hmi_009_offline_close_developer_test.moc"
