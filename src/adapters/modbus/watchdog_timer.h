#pragma once

#include <QtGlobal>

#include <functional>

#include "ports/iplc_gateway.h"

namespace hlm {

// M112 HMI watchdog (spec §8.6). M112 is a hold bit written by the HMI and
// read by the PLC: while the link is valid the HMI flips it every 500 ms so
// the PLC can detect a dead HMI (2 s without an edge clears M42/M106-M111 —
// that is PLC-side behavior, Task 3's simulator already implements it).
//
// Driven by the PLC worker thread (spec §7.2): the owner calls onTick() from
// its own timer, never from the UI event loop. No blocking sleep. A flip
// delayed by higher-priority traffic (queue busy) is still attempted on the
// next tick — the 1 s ack-interval acceptance applies to normal pressure.
// Offline (write rejected) never queues a flip (spec §8.4 no-replay).
//
// Plain class (no QObject): the owner routes the writeCoil callback into the
// gateway's command channel at Heartbeat priority (queue level 3, §8.3).
class WatchdogTimer
{
public:
    // writeCoil returns false when the request was rejected (offline / queue
    // closed): the watchdog then skips the flip and waits for the next tick.
    struct Callbacks {
        std::function<bool(quint16 address, bool value, CommandPriority priority)> writeCoil;
    };

    WatchdogTimer(Callbacks callbacks, std::function<qint64()> nowMs);

    // Online state (spec §8.6: flip only while the connection is valid).
    void setOnline(bool online);

    // Drive the flip timer. Call from the owner's timer tick (no sleep).
    void onTick();

    // M112 protocol address (0-based).
    static constexpr quint16 kM112 = 112;
    // Flip period (spec §8.6).
    static constexpr qint64 kFlipMs = 500;

private:
    Callbacks m_cb;
    std::function<qint64()> m_nowMs;
    bool m_online = false;
    bool m_value = false; // last written value; flips on each tick
    qint64 m_lastFlipMs = 0;
    bool m_hasFlipped = false;
};

} // namespace hlm
