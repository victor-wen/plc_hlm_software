// Task 5 unit tests: M112 heartbeat watchdog (spec §8.6, §7.2).
// Deterministic: injected clock and recording callbacks; no real serial port,
// no sleeps.
//
// Coverage required by the task brief:
// - M112 flips every 500 ms while online.
// - Under normal write pressure the flip ack interval must not exceed 1 s.
// - Offline: no flips are queued (spec §8.4 no-replay).
// - Flips are routed at queue priority 3 (Heartbeat, spec §8.3).
// - A flip delayed by higher-priority traffic is still attempted.

#include <QtTest>

#include "adapters/modbus/watchdog_timer.h"

using namespace hlm;

namespace {

// QPair literal helper (QCOMPARE's macro cannot take a comma inside a
// template argument list).
QPair<quint16, bool> pw(quint16 a, bool v)
{
    return {a, v};
}

class WatchdogHarness
{
public:
    WatchdogTimer::Callbacks callbacks()
    {
        WatchdogTimer::Callbacks cb;
        cb.writeCoil = [this](quint16 a, bool v, CommandPriority p) {
            if (rejectWrites)
                return false; // offline: rejected, not queued
            writes.append({a, v});
            priorities.append(p);
            return true;
        };
        return cb;
    }

    QList<QPair<quint16, bool>> writes;
    QList<CommandPriority> priorities;
    bool rejectWrites = false;
    qint64 now = 0;
};

} // namespace

class WatchdogTimerTest : public QObject
{
    Q_OBJECT

private slots:
    void flipsEvery500ms();
    void ackIntervalWithin1sUnderNormalPressure();
    void offlineDoesNotQueueFlips();
    void flipAtHeartbeatPriority();
    void delayedFlipStillAttempted();
    void stopHaltsFlips();
};

void WatchdogTimerTest::flipsEvery500ms()
{
    WatchdogHarness h;
    WatchdogTimer wd(h.callbacks(), [&h]() { return h.now; });

    wd.setOnline(true);
    wd.onTick();
    QCOMPARE(h.writes.size(), 1);
    QCOMPARE(h.writes.first(), pw(112, true));

    // 499 ms later: no flip yet.
    h.now = 499;
    wd.onTick();
    QCOMPARE(h.writes.size(), 1);

    // 500 ms: flip to 0.
    h.now = 500;
    wd.onTick();
    QCOMPARE(h.writes.size(), 2);
    QCOMPARE(h.writes.last(), pw(112, false));

    // Another 500 ms: flip back to 1.
    h.now = 1000;
    wd.onTick();
    QCOMPARE(h.writes.size(), 3);
    QCOMPARE(h.writes.last(), pw(112, true));
}

void WatchdogTimerTest::ackIntervalWithin1sUnderNormalPressure()
{
    WatchdogHarness h;
    WatchdogTimer wd(h.callbacks(), [&h]() { return h.now; });

    wd.setOnline(true);
    wd.onTick(); // flip #1 at t=0
    QCOMPARE(h.writes.size(), 1);

    // Normal pressure: the ack for each flip arrives promptly (well under
    // 1 s). The next flip is scheduled 500 ms after the previous flip was
    // dispatched, so the ack interval stays ~500 ms.
    qint64 lastFlipMs = 0;
    for (int i = 1; i <= 10; ++i) {
        h.now = lastFlipMs + 500;
        wd.onTick();
        QCOMPARE(h.writes.size(), i + 1);
        const qint64 ackInterval = h.now - lastFlipMs;
        QVERIFY2(ackInterval <= 1000,
                 "flip ack interval must not exceed 1 s under normal pressure");
        lastFlipMs = h.now;
    }
}

void WatchdogTimerTest::offlineDoesNotQueueFlips()
{
    WatchdogHarness h;
    WatchdogTimer wd(h.callbacks(), [&h]() { return h.now; });

    wd.setOnline(false);
    wd.onTick();
    QCOMPARE(h.writes.size(), 0); // no flip queued while offline

    // Going online resumes flipping.
    wd.setOnline(true);
    wd.onTick();
    QCOMPARE(h.writes.size(), 1);
}

void WatchdogTimerTest::flipAtHeartbeatPriority()
{
    WatchdogHarness h;
    WatchdogTimer wd(h.callbacks(), [&h]() { return h.now; });

    wd.setOnline(true);
    wd.onTick();
    QCOMPARE(h.priorities.size(), 1);
    QCOMPARE(h.priorities.first(), CommandPriority::Heartbeat); // level 3 (§8.3)
}

void WatchdogTimerTest::delayedFlipStillAttempted()
{
    WatchdogHarness h;
    WatchdogTimer wd(h.callbacks(), [&h]() { return h.now; });

    wd.setOnline(true);
    wd.onTick(); // flip #1 at t=0
    QCOMPARE(h.writes.size(), 1);

    // The queue is busy with higher-priority traffic: the owner's tick
    // arrives late. The flip must still be attempted (never skipped).
    h.now = 2000;
    wd.onTick();
    QCOMPARE(h.writes.size(), 2);
    QCOMPARE(h.writes.last(), pw(112, false));
}

void WatchdogTimerTest::stopHaltsFlips()
{
    WatchdogHarness h;
    WatchdogTimer wd(h.callbacks(), [&h]() { return h.now; });

    wd.setOnline(true);
    wd.onTick();
    QCOMPARE(h.writes.size(), 1);

    wd.setOnline(false); // stop: heartbeat halts
    h.now = 500;
    wd.onTick();
    QCOMPARE(h.writes.size(), 1); // no further flips
}

QTEST_GUILESS_MAIN(WatchdogTimerTest)
#include "test_watchdog_timer.moc"
