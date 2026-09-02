#pragma once

namespace hlm {

// Reconnect policy (spec §8.4): three consecutive transfer failures (or a
// D140 freeze) take the link offline; offline retries back off 1 s, 2 s, 5 s,
// then every 5 s. A successful reconnect resets the schedule.
class ReconnectPolicy
{
public:
    ReconnectPolicy() = default;

    // Report a transfer failure (read or write). Returns true when the link
    // just transitioned to offline (3 consecutive failures).
    bool onTransferFailure();

    // Report a successful transfer. Resets the consecutive-failure counter.
    void onTransferSuccess();

    // Report a D140 heartbeat freeze (unchanged for 3 s). Forces offline
    // immediately, even if serial data still returns (spec §8.4).
    void onHeartbeatFreeze();

    bool isOffline() const { return m_offline; }

    // Delay before the next reconnect attempt, in milliseconds.
    // 1000, 2000, 5000, then 5000 every time (spec §8.4).
    int nextReconnectDelayMs() const;

    // Called after a reconnect attempt; advances the backoff schedule.
    void onReconnectAttempted();

    // Called after a successful reconnect (a full valid snapshot received).
    // Resets the backoff schedule and the failure counter.
    void onReconnectSucceeded();

    int consecutiveFailures() const { return m_consecutiveFailures; }

private:
    bool m_offline = false;
    int m_consecutiveFailures = 0;
    int m_backoffStep = 0; // 0 -> 1s, 1 -> 2s, 2+ -> 5s
};

} // namespace hlm
