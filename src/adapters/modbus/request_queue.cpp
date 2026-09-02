#include "adapters/modbus/request_queue.h"

namespace hlm {

int RequestQueue::levelOf(const ModbusRequest &req)
{
    switch (req.cls) {
    case RequestClass::PulseClear:
        return 1;
    case RequestClass::SafetyWrite:
        return 2;
    case RequestClass::Heartbeat:
        return 3;
    case RequestClass::UserWrite:
        return 4;
    case RequestClass::FastPoll:
    case RequestClass::HomePoll:
        return 5;
    case RequestClass::CommandPoll:
        return 6;
    case RequestClass::SlowPoll:
        return 7;
    }
    return 7;
}

bool RequestQueue::isPoll(const ModbusRequest &req)
{
    return req.kind == ModbusRequest::Kind::ReadCoils
        || req.kind == ModbusRequest::Kind::ReadRegisters;
}

bool RequestQueue::enqueue(ModbusRequest req)
{
    if (m_closed)
        return false; // offline: no replay of queued commands (spec §8.4)
    req.id = m_nextId++;
    m_requests.append(req);
    return true;
}

bool RequestQueue::next(ModbusRequest &out)
{
    if (m_requests.isEmpty())
        return false;

    // Highest priority first (lowest level); ties broken by insertion order.
    // A poll that has been passed over kAntiStarvationThreshold times is
    // forced to the front (effective level 0) so a continuous write stream
    // can never postpone the fast snapshot indefinitely (spec §8.3).
    int best = 0;
    int bestLevel = effectiveLevel(m_requests.at(0));
    for (int i = 1; i < m_requests.size(); ++i) {
        const int lvl = effectiveLevel(m_requests.at(i));
        if (lvl < bestLevel) {
            best = i;
            bestLevel = lvl;
        }
    }

    ModbusRequest &req = m_requests[best];

    if (isPoll(req)) {
        if (req.skipped >= kAntiStarvationThreshold) {
            out = req;
            m_requests.removeAt(best);
            return true;
        }
        ++req.skipped;
        out = req;
        m_requests.removeAt(best); // consumed; the poll timer re-enqueues it
        return true;
    }

    // A write was dispatched: count it against every pending poll so the
    // fast snapshot cannot be starved by a write burst.
    for (ModbusRequest &p : m_requests) {
        if (isPoll(p))
            ++p.skipped;
    }
    out = req;
    m_requests.removeAt(best);
    return true;
}

int RequestQueue::effectiveLevel(const ModbusRequest &req) const
{
    if (isPoll(req) && req.skipped >= kAntiStarvationThreshold)
        return 0;
    return levelOf(req);
}

void RequestQueue::clear()
{
    m_requests.clear();
}

} // namespace hlm
