#pragma once

#include <QHash>
#include <QtGlobal>

#include <functional>

#include "ports/iplc_gateway.h"

namespace hlm {

// Unified pulse state machine for M101/M102/M103/M43 (spec §8.5). One
// instance serves all pulse addresses; each address runs its own independent
// state machine.
//
// Protocol per pulse (spec §8.5):
//   1. serially write 1 (user-write priority);
//   2. on the write-1 ack, start timing;
//   3. hold high at least 100 ms (queue busy may extend, never shorten);
//   4. enqueue the write-0 at the queue's highest priority (level 1);
//   5. complete when the clear is acked or the readback shows 0.
//
// No blocking sleep: the owner drives onTick() from its own timer. An
// uncertain write (timeout) is never blindly re-sent: the machine reads the
// target bit back, prioritizes ensuring it is 0, then converges per the
// actual state (spec §8.4). Clear has priority over set: a second startPulse
// on an active address is rejected. Offline (write rejected) aborts the
// pulse with finished(false) — commands are never queued for replay
// (spec §8.4).
//
// Plain class (no QObject): the owner (PLC worker thread, spec §7.2) drives
// it directly and routes its callbacks into the gateway's command channel.
class PulseStateMachine
{
public:
    // Transport callbacks. writeCoil/readCoil return false when the request
    // was rejected (offline / queue closed): the machine then aborts rather
    // than queueing anything.
    struct Callbacks {
        std::function<bool(quint16 address, bool value, CommandPriority priority)> writeCoil;
        std::function<bool(quint16 address)> readCoil;
        // Pulse outcome: ok=true when the pulse was delivered and cleared;
        // ok=false when it was aborted (offline, reset, or the set never
        // took effect).
        std::function<void(quint16 address, bool ok)> finished;
    };

    PulseStateMachine(Callbacks callbacks, std::function<qint64()> nowMs);

    // Start a pulse on `address`. Returns false (and reports finished(false))
    // when offline or when a pulse on the same address is already active
    // (clear-before-set priority).
    bool startPulse(quint16 address);

    // Drive the hold timer. Call from the owner's timer tick (no sleep).
    void onTick();

    // Result of the write-1 / write-0 for `address` (from writeCompleted).
    void onWriteCompleted(quint16 address, bool ok);

    // Result of the readback of `address` (from a readback poll).
    void onReadback(quint16 address, bool value);

    // Abort all active pulses (offline / stop). Each aborts with
    // finished(false); nothing is queued.
    void reset();

    bool isActive(quint16 address) const;

    // Minimum high-hold time (spec §8.5).
    static constexpr qint64 kMinHoldMs = 100;

private:
    enum class Phase { Idle, SetInFlight, Holding, ClearInFlight, Readback };

    struct Pulse {
        Phase phase = Phase::Idle;
        qint64 holdStartMs = 0; // clock time of the write-1 ack
        bool uncertain = false; // last write result was uncertain (timeout)
    };

    void abortPulse(quint16 address, bool ok);
    // Returns false when the clear was rejected (offline): the pulse is
    // aborted and its entry removed from m_pulses.
    bool enqueueClear(quint16 address);
    void requestReadback(quint16 address);

    Callbacks m_cb;
    std::function<qint64()> m_nowMs;
    QHash<quint16, Pulse> m_pulses;
};

} // namespace hlm
