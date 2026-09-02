// Task 4 unit tests: single-flight serialized request queue (spec §8.3).
// Deterministic: no real serial port, no sleeps.
//
// Coverage required by the task brief:
// - Priority ordering: pulse clear first, then safety writes, then user
//   writes, then polls (fast < command readback < slow).
// - Anti-starvation: continuous writes must not starve the fast poll.
// - Single-flight: at most one request dispatched at a time.
// - Offline: closed queue rejects enqueue (no replay of commands).

#include <QtTest>

#include "adapters/modbus/request_queue.h"

using namespace hlm;

namespace {

ModbusRequest write(quint16 addr, RequestClass cls = RequestClass::UserWrite)
{
    ModbusRequest r;
    r.kind = ModbusRequest::Kind::WriteCoil;
    r.address = addr;
    r.value = 1;
    r.cls = cls;
    return r;
}

ModbusRequest poll(RequestClass cls)
{
    ModbusRequest r;
    r.kind = (cls == RequestClass::HomePoll || cls == RequestClass::CommandPoll)
        ? ModbusRequest::Kind::ReadCoils
        : ModbusRequest::Kind::ReadRegisters;
    r.address = 100;
    r.count = 41;
    r.cls = cls;
    return r;
}

} // namespace

class RequestQueueTest : public QObject
{
    Q_OBJECT

private slots:
    void pulseClearFirst();
    void safetyBeforeUserWrites();
    void userWritesBeforePolls();
    void pollLevelOrdering();
    void singleFlight();
    void antiStarvation();
    void closedRejectsEnqueue();
    void clearDropsPending();
};

void RequestQueueTest::pulseClearFirst()
{
    RequestQueue q;
    q.enqueue(write(100));                       // user write
    q.enqueue(write(102, RequestClass::SafetyWrite));
    q.enqueue(write(101, RequestClass::PulseClear));

    ModbusRequest r;
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::PulseClear);
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::SafetyWrite);
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::UserWrite);
    QVERIFY(q.isEmpty());
}

void RequestQueueTest::safetyBeforeUserWrites()
{
    RequestQueue q;
    q.enqueue(write(100));
    q.enqueue(write(102, RequestClass::SafetyWrite));

    ModbusRequest r;
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::SafetyWrite);
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::UserWrite);
}

void RequestQueueTest::userWritesBeforePolls()
{
    RequestQueue q;
    q.enqueue(poll(RequestClass::FastPoll));
    q.enqueue(write(100));

    ModbusRequest r;
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::UserWrite);
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::FastPoll);
}

void RequestQueueTest::pollLevelOrdering()
{
    RequestQueue q;
    q.enqueue(poll(RequestClass::SlowPoll));
    q.enqueue(poll(RequestClass::CommandPoll));
    q.enqueue(poll(RequestClass::FastPoll));

    ModbusRequest r;
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::FastPoll);
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::CommandPoll);
    QVERIFY(q.next(r));
    QCOMPARE(r.cls, RequestClass::SlowPoll);
}

void RequestQueueTest::singleFlight()
{
    // next() hands out exactly one request; the queue keeps the rest until
    // the caller returns them (polls are re-queued, writes consumed).
    RequestQueue q;
    q.enqueue(write(100));
    q.enqueue(write(101));

    ModbusRequest r;
    QVERIFY(q.next(r));
    QCOMPARE(q.size(), 1); // one write still pending
    QVERIFY(q.next(r));
    QCOMPARE(q.size(), 0);
    QVERIFY(!q.next(r));
}

void RequestQueueTest::antiStarvation()
{
    // A continuous stream of user writes must not starve the fast poll:
    // after kAntiStarvationThreshold passes-over the poll is forced out.
    RequestQueue q;
    q.enqueue(poll(RequestClass::FastPoll));

    ModbusRequest r;
    int pollsSeen = 0;
    for (int i = 0; i < 20; ++i) {
        q.enqueue(write(quint16(100 + i)));
        if (q.next(r)) {
            if (r.cls == RequestClass::FastPoll)
                ++pollsSeen;
        }
    }
    QVERIFY2(pollsSeen >= 1, "fast poll must not be starved by writes");
}

void RequestQueueTest::closedRejectsEnqueue()
{
    RequestQueue q;
    q.close();
    QVERIFY(!q.enqueue(write(100)));
    QVERIFY(q.isEmpty());
    q.reopen();
    QVERIFY(q.enqueue(write(100)));
}

void RequestQueueTest::clearDropsPending()
{
    RequestQueue q;
    q.enqueue(write(100));
    q.enqueue(write(101));
    q.clear();
    QVERIFY(q.isEmpty());
}

QTEST_GUILESS_MAIN(RequestQueueTest)
#include "test_request_queue.moc"
