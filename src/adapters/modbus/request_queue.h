#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "ports/iplc_gateway.h"

namespace hlm {

// Request class drives the priority level (spec §8.3). Polls are enqueued by
// the gateway itself; writes come from IPlcGateway::writeCoil/writeRegister.
enum class RequestClass {
    UserWrite,    // level 4: other user writes + write-then-readback
    SafetyWrite,  // level 2: online stop / estop set / continuous-motion clear
    PulseClear,   // level 1: pulse clear requests (Task 5)
    Heartbeat,    // level 3: M112 heartbeat flip (Task 5)
    FastPoll,     // level 5: D100-D140 fast status poll
    HomePoll,     // level 5: M50-M53 home-return poll
    CommandPoll,  // level 6: M100-M112 command bit readback
    SlowPoll,     // level 7: D204-D223 slow param poll
};

// One queued Modbus request. Value object; the queue owns it and hands it to
// the transport one at a time (single-flight, spec §8.3).
struct ModbusRequest {
    enum class Kind { ReadCoils, ReadRegisters, WriteCoil, WriteRegister };

    Kind kind = Kind::ReadRegisters;
    quint16 address = 0;   // 0-based protocol address
    quint16 count = 0;     // read count (coils or registers)
    quint16 value = 0;     // write value (coil: 0/1, register: raw)
    bool writeThenReadback = false; // spec §8.4: confirm writes by readback
    bool isReadback = false;        // this read confirms a previous write
    quint64 requestId = 0;          // id of the write this readback confirms
    RequestClass cls = RequestClass::UserWrite;
    quint64 id = 0;        // monotonically increasing, for stable ordering
    int skipped = 0;      // anti-starvation: times a poll was passed over
    int retriesLeft = 0;  // read retries remaining (spec §8.4: retry once)
};

// Single-flight serialized request queue (spec §8.3). At most one request is
// dispatched at a time; the next is popped only after the current one
// completes. Priority levels (high -> low):
//   1. pulse clear, 2. safety writes, 3. M112 heartbeat, 4. user writes,
//   5. fast/home status polls, 6. command readback, 7. slow param poll.
// Anti-starvation: a poll that has been passed over kAntiStarvationThreshold
// times is forced to the front, so a continuous write stream can never
// postpone the fast snapshot indefinitely (spec §8.3).
class RequestQueue
{
public:
    RequestQueue() = default;

    // Enqueue a request. Returns false when the queue is closed (offline:
    // commands must not be queued for later replay, spec §8.4).
    bool enqueue(ModbusRequest req);

    // Enqueue a poll even when the queue is closed. Polls are always allowed:
    // the gateway must be able to fetch a full snapshot during the reconnect
    // window even though writes are rejected (spec §8.4).
    bool enqueuePoll(ModbusRequest req);

    // Pop the highest-priority request. Polls are consumed; the gateway's
    // poll timer re-enqueues them on the next tick. Returns false when empty.
    bool next(ModbusRequest &out);

    bool isEmpty() const { return m_requests.isEmpty(); }
    int size() const { return m_requests.size(); }

    // Close the queue: rejects further enqueue (offline). Reopen to accept
    // again after reconnect.
    void close() { m_closed = true; }
    void reopen() { m_closed = false; }
    bool isClosed() const { return m_closed; }

    // Clear all pending requests (used on reconnect; no replay, spec §8.4).
    void clear();

    // A poll is forced to the front after this many consecutive passes-over.
    static constexpr int kAntiStarvationThreshold = 4;

private:
    static int levelOf(const ModbusRequest &req);
    static bool isPoll(const ModbusRequest &req);
    int effectiveLevel(const ModbusRequest &req) const;

    QList<ModbusRequest> m_requests;
    bool m_closed = false;
    quint64 m_nextId = 1;
};

} // namespace hlm
