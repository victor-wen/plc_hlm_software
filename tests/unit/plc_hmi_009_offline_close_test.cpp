// PLC-HMI-009 OB-2 black-box unit tests (independent): a logical-offline
// transition must actually close the underlying transport, reconnect must
// reopen it, and no command rejected during the offline window may be replayed.
//
// Authored only from .ai/test-briefs/PLC-HMI-009.yaml (brief_version 1), the
// approved .ai/project-contract.yaml and inspectable test sources under
// tests/**. No production implementation source was read (the sandbox denies
// src/** by design); the gateway worker is driven exclusively through its
// public test seams and an injected fake IModbusTransport.
//
// Brief OB-2 observable assertions covered here:
//   * after the offline transition (3 consecutive transfer failures) the
//     transport's open state is false;
//   * after the offline transition caused by a frozen heartbeat (D140 unchanged
//     past the approved window) the transport's open state is false;
//   * on reconnect the transport is opened once more and the gateway returns
//     online after a valid snapshot;
//   * no write that was rejected or dropped during the offline window is
//     replayed afterwards.
//
// Expected RED on the current tree: the logical-offline transition does not
// close the injected transport, so the two close assertions fail until OB-2 is
// implemented; the reconnect/no-replay cases lock the post-fix behavior.

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
// says so, and the reported open state is directly observable.
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
    QList<ModbusRequest> sent;
};

} // namespace

class PlcHmi009OfflineCloseTest : public QObject
{
    Q_OBJECT

private slots:
    void offlineAfterThreeFailuresClosesTheTransport();
    void offlineAfterHeartbeatFreezeClosesTheTransport();
    void reconnectReopensTheTransportAndRejectsWhileOffline();
    void frozenHeartbeatOfflineThenRecoveryKeepsNoQueuedWrites();
    void rejectedWriteStaysAbsentAcrossLaterPolls();
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

    FakeTransport *m_transport = nullptr;
    ModbusGatewayWorker *m_worker = nullptr;
    qint64 m_now = 0;
};

void PlcHmi009OfflineCloseTest::cleanup()
{
    delete m_worker;
    m_worker = nullptr;
    delete m_transport;
    m_transport = nullptr;
    m_now = 0;
}

void PlcHmi009OfflineCloseTest::offlineAfterThreeFailuresClosesTheTransport()
{
    buildWorker();

    // Precondition: a healthy first transfer puts the gateway online with the
    // transport reporting itself open.
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());
    QVERIFY2(m_transport->m_open, "precondition: the online transport reports open");

    // Three consecutive transfer failures; each read retries once, so every
    // failure cycle needs a fresh poll to be dispatched first.
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

    QVERIFY2(!m_worker->isOnline(),
             "precondition: three transfer failures take the gateway offline");

    QVERIFY2(!m_transport->m_open,
             "enterOffline must close the underlying transport so a later reconnect "
             "truly reopens it (OB-2)");
}

void PlcHmi009OfflineCloseTest::offlineAfterHeartbeatFreezeClosesTheTransport()
{
    buildWorker();

    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());
    QVERIFY2(m_transport->m_open, "precondition: the online transport reports open");

    // Healthy transfers, but D140 stops changing: the first unchanged reading
    // is still inside the approved window, the next one freezes the heartbeat
    // and takes the gateway offline.
    m_now = 2900;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    m_now = 3200;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));

    QVERIFY2(!m_worker->isOnline(),
             "precondition: a frozen heartbeat takes the gateway offline");

    QVERIFY2(!m_transport->m_open,
             "a frozen-heartbeat offline transition must close the underlying "
             "transport so a later reconnect truly reopens it (OB-2)");
}

void PlcHmi009OfflineCloseTest::reconnectReopensTheTransportAndRejectsWhileOffline()
{
    buildWorker();

    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

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
    QVERIFY2(!m_transport->m_open,
             "precondition: the offline transition closed the transport");

    // A command submitted while offline must be rejected visibly and must not
    // reach the transport.
    const int sentBeforeRejectedWrite = int(m_transport->sent.size());
    const SubmissionResult rejected =
        m_worker->submitWriteCoil(kM106, true, CommandPriority::Normal);
    QVERIFY2(!rejected.accepted, "a command submitted while offline must be rejected");
    QVERIFY2(!rejected.immediate_rejection_reason.isEmpty(),
             "an offline rejection must carry a visible reason");
    QCOMPARE(int(m_transport->sent.size()), sentBeforeRejectedWrite);

    // Reconnect opens the transport again and a valid snapshot brings the
    // gateway back online.
    m_worker->onReconnectTick();
    QVERIFY2(m_transport->m_open, "reconnect must open the transport again");
    m_transport->completeOk(fastBlock(5));
    QVERIFY(m_worker->isOnline());

    // No replay: after reconnect only the poll that brought the gateway back
    // online is dispatched; the rejected write is not queued.
    QCOMPARE(int(m_transport->sent.size()), sentBeforeRejectedWrite + 1);
}

void PlcHmi009OfflineCloseTest::frozenHeartbeatOfflineThenRecoveryKeepsNoQueuedWrites()
{
    buildWorker();

    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    m_now = 2900;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

    m_now = 3200;
    m_worker->onPollTick();
    m_transport->completeOk(fastBlock(1));
    QVERIFY2(!m_worker->isOnline(),
             "precondition: a frozen heartbeat takes the gateway offline");

    // A command submitted during the frozen-heartbeat offline window must be
    // rejected visibly and must not reach the transport.
    const int sentBeforeRejectedWrite = int(m_transport->sent.size());
    const SubmissionResult rejected =
        m_worker->submitWriteCoil(kM106, true, CommandPriority::Normal);
    QVERIFY2(!rejected.accepted, "a command submitted while offline must be rejected");
    QVERIFY2(!rejected.immediate_rejection_reason.isEmpty(),
             "an offline rejection must carry a visible reason");
    QCOMPARE(int(m_transport->sent.size()), sentBeforeRejectedWrite);

    // Reconnect plus a full valid snapshot restores the online session.
    m_worker->onReconnectTick();
    QVERIFY2(m_transport->m_open, "reconnect must open the transport again");
    m_transport->completeOk(fastBlock(7));
    QVERIFY(m_worker->isOnline());

    // No replay: beyond the poll that restored the session, no further write
    // was sent for the command rejected during the offline window.
    QCOMPARE(int(m_transport->sent.size()), sentBeforeRejectedWrite + 1);
}

// --- OB-2: a rejected write stays absent across later polls ---------------------

void PlcHmi009OfflineCloseTest::rejectedWriteStaysAbsentAcrossLaterPolls()
{
    // Brief OB-2: "No write that was rejected or dropped during the offline
    // window is replayed afterwards." After reconnect and a valid snapshot, the
    // rejected M106 write must not reappear in any subsequent poll burst either.
    buildWorker();

    m_transport->completeOk(fastBlock(1));
    QVERIFY(m_worker->isOnline());

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
    QVERIFY2(!m_worker->isOnline(), "precondition: the gateway went offline");

    const SubmissionResult rejected =
        m_worker->submitWriteCoil(kM106, true, CommandPriority::Normal);
    QVERIFY2(!rejected.accepted, "a command submitted while offline must be rejected");
    QVERIFY2(!rejected.immediate_rejection_reason.isEmpty(),
             "an offline rejection must carry a visible reason");

    // Reconnect, restore the session, then run several later healthy polls.
    m_worker->onReconnectTick();
    QVERIFY2(m_transport->m_open, "reconnect must open the transport again");
    m_transport->completeOk(fastBlock(3));
    QVERIFY(m_worker->isOnline());

    for (int i = 0; i < 3; ++i) {
        m_now += 300;
        m_worker->onPollTick();
        m_transport->completeOk(fastBlock(quint16(4 + i)));
    }
    QVERIFY(m_worker->isOnline());

    for (const ModbusRequest &request : m_transport->sent) {
        QVERIFY2(!(request.kind == ModbusRequest::Kind::WriteCoil
                   && request.address == kM106),
                 "a write rejected during the offline window must never be replayed");
    }
}

QTEST_GUILESS_MAIN(PlcHmi009OfflineCloseTest)
#include "plc_hmi_009_offline_close_test.moc"
