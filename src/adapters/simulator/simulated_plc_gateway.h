#pragma once

#include <QDateTime>
#include <QObject>

#include <optional>

#include "adapters/simulator/h3u_simulation_model.h"
#include "adapters/simulator/simulation_clock.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"

namespace hlm {

// In-process PLC gateway (spec §14.2). Implements the same IPlcGateway port
// as the real Modbus gateway (Task 4) by wrapping the shared H3uSimulationModel
// directly, so application and UI integration tests can swap real<->simulated
// without changing a line of consumer code.
//
// Deterministic by construction: the model clock only advances when tick() is
// called explicitly (default 1 simulated second per tick), so no scenario
// depends on real waits. The observable contract mirrors the real gateway:
//   - snapshotReady() carries complete, atomic DeviceSnapshots built from the
//     model state (fast block D100-D140 via decodeFastBlock, home bits M50-M53,
//     command bits M100-M112, slow block D204/D210/D220).
//   - connectionStateChanged() fires on start/stop, link-down/up and heartbeat
//     freeze (D140 unchanged for 3 ticks, mirroring the real 3 s rule).
//   - writeCoil()/writeRegister() confirm by readback; offline writes are
//     rejected, never queued or replayed.
//
// Test hooks: tick()/setTickSeconds() drive time, setLinkDown() simulates
// 断线/恢复, setHeartbeatFrozen() simulates a dead PLC, model() exposes the
// fault-injection hooks (setPositioningStall, setHomeReturnFault, ...).
class SimulatedPlcGateway : public IPlcGateway
{
    Q_OBJECT

public:
    explicit SimulatedPlcGateway(QObject *parent = nullptr);
    ~SimulatedPlcGateway() override;

    void start() override;
    void stop() override;
    bool isOnline() const override;

    void writeCoil(quint16 address, bool value,
                   CommandPriority priority = CommandPriority::Normal) override;
    void writeRegister(quint16 address, quint16 value,
                       CommandPriority priority = CommandPriority::Normal) override;
    bool startPulse(quint16 address) override;

signals:
    // Communication statistics (spec §16): emitted with every published
    // snapshot. reconnectCount = link-restore count, failedPolls = 0 (the
    // in-process model never drops a poll).
    void commStatsChanged(quint64 sequence, int reconnectCount, int failedPolls);

public:
    // --- deterministic time hooks -------------------------------------------
    // Advance the model by `tickSeconds` simulated seconds and publish a fresh
    // snapshot. No-op while the link is down or the gateway is stopped.
    void tick();
    void setTickSeconds(quint64 seconds) { m_tickSeconds = seconds; }
    quint64 elapsedSeconds() const { return m_clock.elapsed(); }

    // --- link / heartbeat fault hooks ---------------------------------------
    // Simulate 断线/恢复: while down, ticks are ignored, D140 freezes and
    // writes are rejected. Restoring the link reconnects on the next tick.
    void setLinkDown(bool down);
    // Simulate a dead PLC: D140 stops changing; after 3 ticks the gateway
    // goes offline (spec §8.4). Unfreezing reconnects on the next tick.
    void setHeartbeatFrozen(bool frozen);

    // --- snapshot access ----------------------------------------------------
    bool hasSnapshot() const { return m_lastSnapshot.has_value(); }
    DeviceSnapshot lastSnapshot() const { return m_lastSnapshot.value(); }

    // Shared model, exposed for fault injection (setPositioningStall,
    // setHomeReturnFault, setProductionCount).
    H3uSimulationModel &model() { return m_model; }
    const H3uSimulationModel &model() const { return m_model; }

private:
    void publishSnapshot();
    void setOnline(bool online);

    SimulationClock m_clock;
    H3uSimulationModel m_model;

    bool m_started = false;
    bool m_online = false;
    bool m_linkDown = false;
    bool m_heartbeatFrozen = false;
    bool m_offlineDueToFreeze = false;

    quint64 m_tickSeconds = 1;
    quint64 m_sequence = 0;
    // Communication statistics (spec §16).
    int m_reconnectCount = 0;
    int m_failedPolls = 0;

    // D140 heartbeat freeze tracking (spec §8.4): 3 ticks without a change.
    quint64 m_freezeTicks = 0;

    std::optional<DeviceSnapshot> m_lastSnapshot;
};

} // namespace hlm
