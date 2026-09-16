// PLC-HMI-003 developer regression (rework cycle 1): a poll block without a
// successful transfer must become observable as Stale in published snapshots
// once its approved threshold is crossed (fast/home 1000 ms, command 2000 ms,
// slow 4000 ms), even when that block never polls again and no transfer fails.
//
// Regression for the independent finding FAIL-STALE-THRESHOLD-CASE: the worker
// previously published snapshots only on FastPoll success or a failed-transfer
// downgrade, so a frozen fast block was structurally unobservable, and a block
// that never succeeded had its age pinned to zero and stayed ProtocolError
// forever.
//
// The rig advances D140 on every completion so the D140-freeze path (an
// unchanged heartbeat for 3 s takes the link offline) cannot end the session:
// the only variable under test is the per-block evidence age.

#include <QtTest>

#include "adapters/modbus/modbus_transport.h"
#include "adapters/modbus/qt_modbus_plc_gateway.h"

using namespace hlm;

namespace {

constexpr int kStepMs = 50;
constexpr int kFrozenIntervalMs = 1000000;

// Deterministic transport: transfers are completed explicitly by the rig, and
// D140 changes on every completion so the session stays online.
class AdvancingHeartbeatTransport : public IModbusTransport
{
    Q_OBJECT

public:
    bool open() override { return true; }
    void close() override {}
    bool isOpen() const override { return true; }

    bool send(const ModbusRequest &req) override
    {
        sent.append(req);
        return true;
    }

    void completeOk()
    {
        TransferResult res;
        res.ok = true;
        res.values = QList<quint16>(41, 0); // fast/home/command/slow sizes are all <= 41
        res.values[40] = ++m_heartbeat;
        emit transferFinished(res);
    }

    QList<ModbusRequest> sent;

private:
    quint16 m_heartbeat = 0;
};

// Worker on an injected clock. One block's interval can be frozen far above
// its stale threshold while the other blocks keep refreshing every 250 ms.
struct WorkerRig
{
    WorkerRig(int fastMs, int homeMs, int commandMs, int slowMs)
        : worker(QtModbusPlcGateway::Config(), &transport)
    {
        worker.setNowMs([this]() { return now; });
        worker.setPollIntervals(fastMs, homeMs, commandMs, slowMs);
        QObject::connect(&worker, &ModbusGatewayWorker::snapshotReady, &worker,
                         [this](quint64, const DeviceSnapshot &s) {
                             snapshots.append(s);
                         });
        worker.start();
        transport.completeOk(); // the immediate first fast poll -> online
        completed = static_cast<int>(transport.sent.size());
        Q_ASSERT(worker.isOnline());
    }

    void drive(int steps)
    {
        for (int i = 0; i < steps; ++i) {
            now += kStepMs;
            worker.onPollTick();
            while (transport.sent.size() > completed) {
                transport.completeOk();
                ++completed;
            }
        }
    }

    AdvancingHeartbeatTransport transport;
    ModbusGatewayWorker worker;
    qint64 now = 0;
    int completed = 0;
    QVector<DeviceSnapshot> snapshots;
};

} // namespace

class PlcStalePublicationDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void frozenFastBlockBecomesStaleWhileOthersStayValid();
    void neverSucceededBlockBecomesStaleWithoutItsOwnTransfer();
    void commandAndSlowBlocksBecomeStaleAtTheirThresholds();
    void staleTransitionPublishesExactlyOnceAndRecoversAfterSuccess();
};

void PlcStalePublicationDeveloperTest::frozenFastBlockBecomesStaleWhileOthersStayValid()
{
    WorkerRig rig(kFrozenIntervalMs, 250, 250, 250);
    rig.drive(24); // now = 1200 ms, past the 1000 ms fast threshold

    bool sawYoungAndNotStale = false;
    bool sawOldAndStale = false;
    qint64 previousAge = -1;
    for (const DeviceSnapshot &s : rig.snapshots) {
        QVERIFY2(s.fast_age_ms >= previousAge,
                 "the fast block age must be monotonic elapsed time");
        previousAge = s.fast_age_ms;
        if (s.fast_age_ms < kFastStaleMs) {
            QVERIFY2(s.fast_quality != DataQuality::Stale,
                     "a fast block younger than its threshold must not be stale");
            sawYoungAndNotStale = true;
        } else if (s.fast_age_ms > kFastStaleMs) {
            QVERIFY2(s.fast_quality == DataQuality::Stale,
                     "a frozen fast block must become Stale once its threshold is crossed");
            QVERIFY2(s.home_quality == DataQuality::Valid
                         && s.command_quality == DataQuality::Valid
                         && s.slow_quality == DataQuality::Valid,
                     "only the block without a successful transfer may go stale");
            sawOldAndStale = true;
        }
    }
    QVERIFY2(sawYoungAndNotStale,
             "no published snapshot was younger than the fast stale threshold");
    QVERIFY2(sawOldAndStale, "the frozen fast block was never published as Stale");
}

void PlcStalePublicationDeveloperTest::neverSucceededBlockBecomesStaleWithoutItsOwnTransfer()
{
    WorkerRig rig(250, kFrozenIntervalMs, 250, 250);
    rig.drive(24); // now = 1200 ms, past the 1000 ms home threshold

    for (const ModbusRequest &r : rig.transport.sent) {
        QVERIFY2(r.cls != RequestClass::HomePoll,
                 "precondition: the home block never got a transfer of its own");
    }

    const DeviceSnapshot &last = rig.snapshots.last();
    QVERIFY2(last.home_age_ms > kHomeStaleMs,
             "a block without any success must still age past its threshold");
    QVERIFY2(last.home_quality == DataQuality::Stale,
             "a block without success beyond its threshold must be Stale");
    QVERIFY2(last.fast_quality == DataQuality::Valid,
             "the refreshing fast block must stay valid");
}

void PlcStalePublicationDeveloperTest::commandAndSlowBlocksBecomeStaleAtTheirThresholds()
{
    {
        WorkerRig rig(250, 250, kFrozenIntervalMs, 250);
        rig.drive(44); // now = 2200 ms, past the 2000 ms command threshold
        const DeviceSnapshot &last = rig.snapshots.last();
        QVERIFY(last.command_age_ms > kCommandStaleMs);
        QVERIFY2(last.command_quality == DataQuality::Stale,
                 "a frozen command block must become Stale after 2000 ms without success");
        QVERIFY2(last.fast_quality == DataQuality::Valid,
                 "the refreshing fast block must stay valid");
    }
    {
        WorkerRig rig(250, 250, 250, kFrozenIntervalMs);
        rig.drive(84); // now = 4200 ms, past the 4000 ms slow threshold
        const DeviceSnapshot &last = rig.snapshots.last();
        QVERIFY(last.slow_age_ms > kSlowStaleMs);
        QVERIFY2(last.slow_quality == DataQuality::Stale,
                 "a frozen slow block must become Stale after 4000 ms without success");
        QVERIFY2(last.fast_quality == DataQuality::Valid,
                 "the refreshing fast block must stay valid");
    }
}

void PlcStalePublicationDeveloperTest::staleTransitionPublishesExactlyOnceAndRecoversAfterSuccess()
{
    WorkerRig rig(kFrozenIntervalMs, 250, 250, 250);
    rig.drive(21); // now = 1050 ms: the single threshold-crossing publication

    QCOMPARE(rig.snapshots.size(), qsizetype(2)); // initial + crossing, nothing else
    QVERIFY(rig.snapshots.last().fast_quality == DataQuality::Stale);

    rig.drive(10); // stale quality unchanged: must not re-publish
    QCOMPARE(rig.snapshots.size(), qsizetype(2));

    // Re-enable the fast poll: the next successful transfer clears the block.
    rig.worker.setPollIntervals(250, 250, 250, 250);
    rig.drive(6);
    const DeviceSnapshot &last = rig.snapshots.last();
    QVERIFY2(last.fast_quality == DataQuality::Valid,
             "a successful refresh must clear the stale fast block");
    QVERIFY(last.fast_age_ms <= kFastStaleMs);
}

QTEST_GUILESS_MAIN(PlcStalePublicationDeveloperTest)
#include "plc_stale_publication_developer_test.moc"
