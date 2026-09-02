// Task 4 unit tests: reconnect policy (spec §8.4). Deterministic, no sleeps.
//
// Coverage required by the task brief:
// - Three consecutive transfer failures -> offline.
// - 1 s / 2 s / 5 s backoff schedule, then every 5 s.
// - Reconnect success resets the schedule.
// - D140 heartbeat freeze forces offline immediately.

#include <QtTest>

#include "adapters/modbus/reconnect_policy.h"

using namespace hlm;

class ReconnectPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void threeFailuresGoOffline();
    void successResetsFailureCounter();
    void backoffSchedule();
    void reconnectSuccessResetsSchedule();
    void heartbeatFreezeForcesOffline();
    void heartbeatFreezeResetsBackoff();
};

void ReconnectPolicyTest::threeFailuresGoOffline()
{
    ReconnectPolicy p;
    QVERIFY(!p.isOffline());
    QVERIFY(!p.onTransferFailure());
    QVERIFY(!p.isOffline());
    QVERIFY(!p.onTransferFailure());
    QVERIFY(!p.isOffline());
    QVERIFY(p.onTransferFailure()); // third consecutive failure
    QVERIFY(p.isOffline());
    QCOMPARE(p.consecutiveFailures(), 3);
}

void ReconnectPolicyTest::successResetsFailureCounter()
{
    ReconnectPolicy p;
    p.onTransferFailure();
    p.onTransferFailure();
    p.onTransferSuccess();
    QVERIFY(!p.isOffline());
    QCOMPARE(p.consecutiveFailures(), 0);
    // Two more failures after the reset must not yet go offline.
    p.onTransferFailure();
    p.onTransferFailure();
    QVERIFY(!p.isOffline());
}

void ReconnectPolicyTest::backoffSchedule()
{
    ReconnectPolicy p;
    p.onTransferFailure();
    p.onTransferFailure();
    p.onTransferFailure();
    QVERIFY(p.isOffline());

    QCOMPARE(p.nextReconnectDelayMs(), 1000);
    p.onReconnectAttempted();
    QCOMPARE(p.nextReconnectDelayMs(), 2000);
    p.onReconnectAttempted();
    QCOMPARE(p.nextReconnectDelayMs(), 5000);
    p.onReconnectAttempted();
    QCOMPARE(p.nextReconnectDelayMs(), 5000); // every 5 s from here on
    p.onReconnectAttempted();
    QCOMPARE(p.nextReconnectDelayMs(), 5000);
}

void ReconnectPolicyTest::reconnectSuccessResetsSchedule()
{
    ReconnectPolicy p;
    p.onTransferFailure();
    p.onTransferFailure();
    p.onTransferFailure();
    p.onReconnectAttempted();
    p.onReconnectAttempted();
    QCOMPARE(p.nextReconnectDelayMs(), 5000);

    p.onReconnectSucceeded();
    QVERIFY(!p.isOffline());
    QCOMPARE(p.consecutiveFailures(), 0);
    QCOMPARE(p.nextReconnectDelayMs(), 1000); // schedule restarted
}

void ReconnectPolicyTest::heartbeatFreezeForcesOffline()
{
    ReconnectPolicy p;
    QVERIFY(!p.isOffline());
    p.onHeartbeatFreeze();
    QVERIFY(p.isOffline());
    QCOMPARE(p.consecutiveFailures(), 0); // freeze is not a transfer failure
}

void ReconnectPolicyTest::heartbeatFreezeResetsBackoff()
{
    ReconnectPolicy p;
    p.onTransferFailure();
    p.onTransferFailure();
    p.onTransferFailure();
    p.onReconnectAttempted();
    p.onReconnectAttempted();
    QCOMPARE(p.nextReconnectDelayMs(), 5000);

    p.onHeartbeatFreeze();
    QCOMPARE(p.nextReconnectDelayMs(), 1000); // backoff restarted
}

QTEST_GUILESS_MAIN(ReconnectPolicyTest)
#include "test_reconnect_policy.moc"
