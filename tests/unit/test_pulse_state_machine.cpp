// Task 5 unit tests: unified pulse state machine for M101/M102/M103/M43
// (spec §8.4, §8.5). Deterministic: injected clock and recording callbacks;
// no real serial port, no sleeps.
//
// Coverage required by the task brief:
// - Write-1 confirmed -> hold at least 100 ms before the clear is enqueued.
// - Clear is enqueued at the queue's highest priority (PulseClear, level 1).
// - An uncertain write-1 (timeout) is never blindly re-sent: the machine
//   reads the target bit back, prioritizes ensuring it is 0, then converges.
// - Clear has priority over set (a second startPulse while active is rejected).
// - Offline: pulse commands are rejected, not queued (spec §8.4 no-replay).
// - reset() aborts active pulses (offline / stop) with finished(false).

#include <QtTest>

#include "adapters/modbus/pulse_state_machine.h"

using namespace hlm;

namespace {

// QPair literal helper (QCOMPARE's macro cannot take a comma inside a
// template argument list).
QPair<quint16, bool> pw(quint16 a, bool v)
{
    return {a, v};
}

// Recording transport callbacks; the test drives the machine's inputs.
class PulseHarness
{
public:
    PulseStateMachine::Callbacks callbacks()
    {
        PulseStateMachine::Callbacks cb;
        cb.writeCoil = [this](quint16 a, bool v, CommandPriority p) {
            if (rejectWrites)
                return false; // offline: rejected, not queued
            writes.append({a, v});
            priorities.append(p);
            return true;
        };
        cb.readCoil = [this](quint16 a) {
            if (rejectReads)
                return false;
            reads.append(a);
            return true;
        };
        cb.finished = [this](quint16 a, bool ok) { finished.append({a, ok}); };
        return cb;
    }

    QList<QPair<quint16, bool>> writes;
    QList<CommandPriority> priorities;
    QList<quint16> reads;
    QList<QPair<quint16, bool>> finished;
    bool rejectWrites = false;
    bool rejectReads = false;
    qint64 now = 0;
};

} // namespace

class PulseStateMachineTest : public QObject
{
    Q_OBJECT

private slots:
    void holdAtLeast100ms();
    void clearAtHighestPriority();
    void uncertainWriteDoesNotResendOne();
    void uncertainSetReadbackZeroFinishesFailed();
    void clearFailureConvergesViaReadback();
    void clearFailureReadbackZeroCompletes();
    void offlineRejectsPulse();
    void offlineMidPulseAborts();
    void clearBeforeSetPriority();
    void resetAbortsActivePulses();
    void lateTickExtendsHold();
};

void PulseStateMachineTest::holdAtLeast100ms()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    QCOMPARE(h.writes.size(), 1);
    QCOMPARE(h.writes.first(), pw(101, true));

    // Write-1 confirmed: timing starts now.
    sm.onWriteCompleted(101, true);
    QVERIFY(sm.isActive(101));

    // Before 100 ms the clear must NOT be enqueued.
    h.now = 50;
    sm.onTick();
    QCOMPARE(h.writes.size(), 1);
    h.now = 99;
    sm.onTick();
    QCOMPARE(h.writes.size(), 1);

    // At exactly 100 ms the clear is enqueued.
    h.now = 100;
    sm.onTick();
    QCOMPARE(h.writes.size(), 2);
    QCOMPARE(h.writes.last(), pw(101, false));

    // Clear confirmed: pulse complete.
    sm.onWriteCompleted(101, true);
    QCOMPARE(h.finished.size(), 1);
    QCOMPARE(h.finished.first(), pw(101, true));
    QVERIFY(!sm.isActive(101));
}

void PulseStateMachineTest::clearAtHighestPriority()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(43));
    QCOMPARE(h.priorities.first(), CommandPriority::Normal); // set is a user write

    sm.onWriteCompleted(43, true);
    h.now = 100;
    sm.onTick();

    // The clear must go to the queue's highest priority (level 1, §8.3).
    QCOMPARE(h.writes.size(), 2);
    QCOMPARE(h.priorities.last(), CommandPriority::PulseClear);
}

void PulseStateMachineTest::uncertainWriteDoesNotResendOne()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    QCOMPARE(h.writes.size(), 1);

    // Write-1 times out (uncertain result, spec §8.4): the machine must NOT
    // blindly re-send 1. It reads the target bit back instead.
    sm.onWriteCompleted(101, false);
    QCOMPARE(h.writes.size(), 1); // no re-send of 1
    QCOMPARE(h.reads.size(), 1);
    QCOMPARE(h.reads.first(), quint16(101));

    // Readback shows the bit is still 1: prioritize ensuring it is 0.
    sm.onReadback(101, true);
    QCOMPARE(h.writes.size(), 2);
    QCOMPARE(h.writes.last(), pw(101, false));
    QCOMPARE(h.priorities.last(), CommandPriority::PulseClear);

    sm.onWriteCompleted(101, true);
    QCOMPARE(h.finished.size(), 1);
    QCOMPARE(h.finished.first(), pw(101, true));
}

void PulseStateMachineTest::uncertainSetReadbackZeroFinishesFailed()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(102));
    sm.onWriteCompleted(102, false); // uncertain write-1
    QCOMPARE(h.reads.size(), 1);

    // Readback shows 0: the set never took effect, nothing to clear. The
    // pulse is aborted and reported as not executed.
    sm.onReadback(102, false);
    QCOMPARE(h.writes.size(), 1); // no clear write
    QCOMPARE(h.finished.size(), 1);
    QCOMPARE(h.finished.first(), pw(102, false));
    QVERIFY(!sm.isActive(102));
}

void PulseStateMachineTest::clearFailureConvergesViaReadback()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    sm.onWriteCompleted(101, true);
    h.now = 100;
    sm.onTick();
    QCOMPARE(h.writes.size(), 2); // clear dispatched

    // Clear times out (uncertain): read back, do not assume anything.
    sm.onWriteCompleted(101, false);
    QCOMPARE(h.reads.size(), 1);

    // Bit still 1: re-clear (prioritize ensuring it is 0).
    sm.onReadback(101, true);
    QCOMPARE(h.writes.size(), 3);
    QCOMPARE(h.writes.last(), pw(101, false));
    QCOMPARE(h.priorities.last(), CommandPriority::PulseClear);

    sm.onWriteCompleted(101, true);
    QCOMPARE(h.finished.size(), 1);
    QCOMPARE(h.finished.first(), pw(101, true));
}

void PulseStateMachineTest::clearFailureReadbackZeroCompletes()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    sm.onWriteCompleted(101, true);
    h.now = 100;
    sm.onTick();

    // Clear times out; readback shows the bit is already 0: the pulse is
    // complete (spec §8.5 step 5: 收到清零应答或回读为 0 后完成脉冲).
    sm.onWriteCompleted(101, false);
    sm.onReadback(101, false);
    QCOMPARE(h.finished.size(), 1);
    QCOMPARE(h.finished.first(), pw(101, true));
    QVERIFY(!sm.isActive(101));
}

void PulseStateMachineTest::offlineRejectsPulse()
{
    PulseHarness h;
    h.rejectWrites = true; // queue closed (offline)
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    // Offline: the pulse is rejected, not queued (spec §8.4 no-replay).
    QVERIFY(!sm.startPulse(101));
    QCOMPARE(h.writes.size(), 0);
    QCOMPARE(h.finished.size(), 1);
    QCOMPARE(h.finished.first(), pw(101, false));
    QVERIFY(!sm.isActive(101));
}

void PulseStateMachineTest::offlineMidPulseAborts()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    sm.onWriteCompleted(101, true); // holding

    // Link drops while holding: the clear write is rejected -> abort.
    h.rejectWrites = true;
    h.now = 100;
    sm.onTick();
    QCOMPARE(h.finished.size(), 1);
    QCOMPARE(h.finished.first(), pw(101, false));
    QVERIFY(!sm.isActive(101));
}

void PulseStateMachineTest::clearBeforeSetPriority()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    sm.onWriteCompleted(101, true); // holding

    // A second start on the same address while the pulse is active is
    // rejected: the active pulse's clear has priority over a new set.
    QVERIFY(!sm.startPulse(101));
    QCOMPARE(h.writes.size(), 1); // no second write-1

    h.now = 100;
    sm.onTick();
    QCOMPARE(h.writes.size(), 2); // the original pulse's clear still fires
    QCOMPARE(h.writes.last(), pw(101, false));
}

void PulseStateMachineTest::resetAbortsActivePulses()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    sm.onWriteCompleted(101, true); // holding
    QVERIFY(sm.startPulse(102));    // write-1 in flight

    sm.reset(); // offline / stop
    QCOMPARE(h.finished.size(), 2);
    // QHash iteration order is unspecified: check both entries, not order.
    QVERIFY(h.finished.contains(pw(101, false)));
    QVERIFY(h.finished.contains(pw(102, false)));
    QVERIFY(!sm.isActive(101));
    QVERIFY(!sm.isActive(102));

    // A late tick must not resurrect anything.
    h.now = 100;
    sm.onTick();
    QCOMPARE(h.writes.size(), 2);
}

void PulseStateMachineTest::lateTickExtendsHold()
{
    PulseHarness h;
    PulseStateMachine sm(h.callbacks(), [&h]() { return h.now; });

    QVERIFY(sm.startPulse(101));
    sm.onWriteCompleted(101, true);

    // The owner's timer tick arrives late (queue busy / coarse timer): the
    // high level is extended, never shortened below 100 ms (spec §8.5).
    h.now = 250;
    sm.onTick();
    QCOMPARE(h.writes.size(), 2);
    QCOMPARE(h.writes.last(), pw(101, false));
}

QTEST_GUILESS_MAIN(PulseStateMachineTest)
#include "test_pulse_state_machine.moc"
