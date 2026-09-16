#include "adapters/modbus/pulse_state_machine.h"

namespace hlm {

PulseStateMachine::PulseStateMachine(Callbacks callbacks, std::function<qint64()> nowMs)
    : m_cb(std::move(callbacks))
    , m_nowMs(std::move(nowMs))
{
}

bool PulseStateMachine::startPulse(quint16 address, quint64 submissionId)
{
    // Clear has priority over set: a pulse already active on this address
    // must not be disturbed by a new set (spec §8.5).
    if (m_pulses.contains(address))
        return false;

    // Offline: rejected, never queued for replay (spec §8.4). A rejected
    // submission emits no completion (contract IPlcGateway submission rule).
    if (!m_cb.writeCoil || !m_cb.writeCoil(address, true, CommandPriority::Normal))
        return false;

    Pulse p;
    p.phase = Phase::SetInFlight;
    p.submissionId = submissionId;
    m_pulses.insert(address, p);
    return true;
}

void PulseStateMachine::onTick()
{
    const qint64 now = m_nowMs();
    for (auto it = m_pulses.begin(); it != m_pulses.end();) {
        Pulse &p = it.value();
        if (p.phase == Phase::Holding && now - p.holdStartMs >= kMinHoldMs) {
            // enqueueClear returns false when the clear was rejected (offline):
            // it aborts the pulse and removes its entry from m_pulses, which
            // invalidates `it` and `p`. We must not touch either afterwards, so
            // we restart the iteration from the beginning (the entry is gone).
            if (!enqueueClear(it.key())) {
                it = m_pulses.begin();
                continue;
            }
            // Clear accepted: the pulse is now ClearInFlight; keep iterating.
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
            const quint64 submissionId = p.submissionId;
            m_pulses.erase(it);
            if (m_cb.finished)
                m_cb.finished(address, true, submissionId);
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
    // Copy before erase: erasing the entry invalidates `p`.
    const bool uncertain = p.uncertain;
    const quint64 submissionId = p.submissionId;
    m_pulses.erase(it);
    if (m_cb.finished)
        m_cb.finished(address, !uncertain, submissionId);
}

void PulseStateMachine::reset()
{
    const QHash<quint16, Pulse> pulses = m_pulses;
    m_pulses.clear();
    for (auto it = pulses.constBegin(); it != pulses.constEnd(); ++it) {
        if (m_cb.finished)
            m_cb.finished(it.key(), false, it.value().submissionId);
    }
}

bool PulseStateMachine::isActive(quint16 address) const
{
    return m_pulses.contains(address);
}

void PulseStateMachine::abortPulse(quint16 address, bool ok)
{
    const auto it = m_pulses.constFind(address);
    const quint64 submissionId = it != m_pulses.constEnd() ? it.value().submissionId : 0;
    m_pulses.remove(address);
    if (m_cb.finished)
        m_cb.finished(address, ok, submissionId);
}

bool PulseStateMachine::enqueueClear(quint16 address)
{
    // Clear goes to the queue's highest priority (level 1, spec §8.3).
    if (!m_cb.writeCoil || !m_cb.writeCoil(address, false, CommandPriority::PulseClear)) {
        abortPulse(address, false); // offline: abort, never queue
        return false;
    }
    auto it = m_pulses.find(address);
    if (it != m_pulses.end())
        it.value().phase = Phase::ClearInFlight;
    return true;
}

void PulseStateMachine::requestReadback(quint16 address)
{
    if (!m_cb.readCoil || !m_cb.readCoil(address)) {
        abortPulse(address, false); // offline: abort, never queue
        return;
    }
}

} // namespace hlm
