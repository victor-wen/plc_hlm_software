#include "adapters/modbus/watchdog_timer.h"

namespace hlm {

WatchdogTimer::WatchdogTimer(Callbacks callbacks, std::function<qint64()> nowMs)
    : m_cb(std::move(callbacks))
    , m_nowMs(std::move(nowMs))
{
}

void WatchdogTimer::setOnline(bool online)
{
    m_online = online;
    if (!m_online)
        m_hasFlipped = false; // re-arm on the next online transition
}

void WatchdogTimer::onTick()
{
    if (!m_online)
        return; // offline: no flips, nothing queued (spec §8.4)

    const qint64 now = m_nowMs();
    if (m_hasFlipped && now - m_lastFlipMs < kFlipMs)
        return; // not yet due

    // Flip M112 (spec §8.6). A flip delayed by higher-priority traffic is
    // still attempted here; a rejected write (offline) simply waits for the
    // next tick — never queued for replay.
    m_value = !m_value;
    m_lastFlipMs = now;
    m_hasFlipped = true;
    if (m_cb.writeCoil)
        m_cb.writeCoil(kM112, m_value, CommandPriority::Heartbeat); // level 3 (§8.3)
}

} // namespace hlm
