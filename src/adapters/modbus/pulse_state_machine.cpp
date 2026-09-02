#include "adapters/modbus/pulse_state_machine.h"

namespace hlm {

PulseStateMachine::PulseStateMachine(Callbacks callbacks, std::function<qint64()> nowMs)
    : m_cb(std::move(callbacks))
    , m_nowMs(std::move(nowMs))
{
}

bool PulseStateMachine::startPulse(quint16 address)
{
    // Clear has priority over set: a pulse already active on this address
    // must not be disturbed by a new set (spec §8.5).
    if (m_pulses.contains(address))
        return false;

    // Offline: rejected, never queued for replay (spec §8.4).
    if (!m_cb.writeCoil || !m_cb.writeCoil(address, true, CommandPriority::Normal)) {
        abortPulse(address, false);
        return false;
    }

    Pulse p;
    p.phase = Phase::SetInFlight;
    m_pulses.insert(address, p);
    return true;
}

void PulseStateMachine::onTick()
{
    const qint64 now = m_nowMs();
    for (auto it = m_pulses.begin(); it != m_pulses.end();) {
        Pulse &p = it.value();
        if (p.phase == Phase::Holding && now - p.holdStartMs >= kMinHoldMs) {
            enqueueClear(it.key());
            if (p.phase == Phase::Idle) {
                it = m_pulses.erase(it); // aborted by enqueueClear
                continue;
            }
        }
        ++it;
    }
}

void PulseStateMachine::onWriteCompleted(quint16 address, bool ok)
{
    auto it = m_pulses.find(address);
    if (it == m_pulses.end())
        return;

    Pulse &p = it.value();
    switch (p.phase) {
    case Phase::SetInFlight:
        if (ok) {
            // Write-1 acked: start the hold timing (spec §8.5 step 2).
            p.phase = Phase::Holding;
            p.holdStartMs = m_nowMs();
        } else {
            // Uncertain write-1 (timeout): never blindly re-send 1. Read the
            // target bit back and converge per the actual state (spec §8.4).
            p.uncertain = true;
            p.phase = Phase::Readback;
            requestReadback(address);
        }
        break;
    case Phase::ClearInFlight:
        if (ok) {
            // Clear acked: pulse complete (spec §8.5 step 5).
            m_pulses.erase(it);
            if (m_cb.finished)
                m_cb.finished(address, true);
        } else {
            // Uncertain clear: read back and converge (spec §8.4).
            p.phase = Phase::Readback;
            requestReadback(address);
        }
        break;
    default:
        break; // stale result for a phase that no longer waits on it
    }
}

void PulseStateMachine::onReadback(quint16 address, bool value)
{
    auto it = m_pulses.find(address);
    if (it == m_pulses.end())
        return;

    Pulse &p = it.value();
    if (p.phase != Phase::Readback)
        return;

    if (value) {
        // Bit still 1: prioritize ensuring it is 0 (spec §8.4).
        enqueueClear(address);
        return;
    }

    // Bit is 0. For an uncertain set this means the set never took effect:
    // the pulse was not delivered. For an uncertain clear it means the pulse
    // is complete (spec §8.5 step 5: 收到清零应答或回读为 0 后完成脉冲).
    m_pulses.erase(it);
    if (m_cb.finished)
        m_cb.finished(address, !p.uncertain);
}

void PulseStateMachine::reset()
{
    const auto addresses = m_pulses.keys();
    m_pulses.clear();
    for (quint16 a : addresses) {
        if (m_cb.finished)
            m_cb.finished(a, false);
    }
}

bool PulseStateMachine::isActive(quint16 address) const
{
    return m_pulses.contains(address);
}

void PulseStateMachine::abortPulse(quint16 address, bool ok)
{
    m_pulses.remove(address);
    if (m_cb.finished)
        m_cb.finished(address, ok);
}

void PulseStateMachine::enqueueClear(quint16 address)
{
    // Clear goes to the queue's highest priority (level 1, spec §8.3).
    if (!m_cb.writeCoil || !m_cb.writeCoil(address, false, CommandPriority::PulseClear)) {
        abortPulse(address, false); // offline: abort, never queue
        return;
    }
    auto it = m_pulses.find(address);
    if (it != m_pulses.end())
        it.value().phase = Phase::ClearInFlight;
}

void PulseStateMachine::requestReadback(quint16 address)
{
    if (!m_cb.readCoil || !m_cb.readCoil(address)) {
        abortPulse(address, false); // offline: abort, never queue
        return;
    }
}

} // namespace hlm
