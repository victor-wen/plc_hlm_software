#include "adapters/modbus/reconnect_policy.h"

namespace hlm {

bool ReconnectPolicy::onTransferFailure()
{
    ++m_consecutiveFailures;
    if (m_consecutiveFailures >= 3) {
        m_offline = true;
        m_backoffStep = 0;
        return true;
    }
    return false;
}

void ReconnectPolicy::onTransferSuccess()
{
    m_consecutiveFailures = 0;
}

void ReconnectPolicy::onHeartbeatFreeze()
{
    m_offline = true;
    m_backoffStep = 0;
}

int ReconnectPolicy::nextReconnectDelayMs() const
{
    switch (m_backoffStep) {
    case 0:
        return 1000;
    case 1:
        return 2000;
    default:
        return 5000;
    }
}

void ReconnectPolicy::onReconnectAttempted()
{
    ++m_backoffStep;
}

void ReconnectPolicy::onReconnectSucceeded()
{
    m_offline = false;
    m_consecutiveFailures = 0;
    m_backoffStep = 0;
}

} // namespace hlm
