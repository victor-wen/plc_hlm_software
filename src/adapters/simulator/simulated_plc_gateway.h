#pragma once

#include <QDateTime>
#include <QObject>
#include <QVector>

#include <optional>

#include "adapters/simulator/h3u_simulation_model.h"
#include "adapters/simulator/simulation_clock.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"

namespace hlm {

// In-process PLC gateway (spec §14.2). Implements the revised IPlcGateway port
// (PLC-HMI-003 D1/D8) by wrapping the shared H3uSimulationModel directly, so
// application and UI integration tests can swap real<->simulated without
// changing a line of consumer code.
//
// Deterministic by construction: the model clock only advances when tick() is
// called explicitly (default 1 simulated second per tick), so no scenario
// depends on real waits. The observable contract mirrors the real gateway:
//   - submitWriteCoil/submitWriteRegister/submitPulse return a structured
//     SubmissionResult with a request id unique within the current gateway
//     generation; every accepted submission emits exactly one correlated
//     submissionCompleted() on the next tick.
//   - snapshotReady() carries the gateway generation plus a complete, atomic
//     DeviceSnapshot built from the model state (fast block D100-D140 via
//     decodeFastBlock, home bits M50-M53, command bits M100-M111, slow block
//     D204/D210/D220, real per-block quality/ages).
//   - connectionStateChanged() carries the gateway generation and fires on
//     start/stop, link-down/up and heartbeat freeze (D140 unchanged for 3
//     ticks, mirroring the real 3 s rule). Reconnecting advances the
//     generation.
//   - commStatsChanged() is emitted with every published snapshot.
//   - offline/replaced submissions are rejected, never queued or replayed;
//     accepted-but-unfinished submissions converge as failed instead.
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
    void setGatewayGeneration(quint64 generation) override;
    quint64 gatewayGeneration() const override;

    SubmissionResult submitWriteCoil(
        quint16 address, bool value,
        CommandPriority priority = CommandPriority::Normal) override;
    SubmissionResult submitWriteRegister(
        quint16 address, quint16 value,
        CommandPriority priority = CommandPriority::Normal) override;
    SubmissionResult submitPulse(quint16 address) override;

public:
    // --- deterministic time hooks -------------------------------------------
    // Advance the model by `tickSeconds` simulated seconds, publish a fresh
    // snapshot and deliver correlated completions. No-op while the link is
    // down or the gateway is stopped.
    void tick();
    void setTickSeconds(quint64 seconds) { m_tickSeconds = seconds; }
    quint64 elapsedSeconds() const { return m_clock.elapsed(); }

    // --- link / heartbeat fault hooks ---------------------------------------
    // Simulate 断线/恢复: while down, ticks are ignored, D140 freezes and
    // submissions are rejected. Restoring the link reconnects on the next
    // tick and advances the gateway generation.
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

    // Synchronous fixture helpers that stage raw model state. They are NOT
    // part of the revised IPlcGateway port; new tests use model() or the
    // submission API. Kept so earlier integration fixtures that stage the
    // machine (home ready, auto mode) remain valid.
    void writeCoil(quint16 address, bool value) { m_model.writeCoil(address, value); }
    void writeRegister(quint16 address, quint16 value)
    {
        m_model.writeRegister(address, value);
    }

private:
    struct PendingSubmission {
        quint64 request_id = 0;
        quint64 gateway_generation = 0;
        PlcOperation operation = PlcOperation::WriteCoil;
        quint16 address = 0;
        bool coilValue = false;
        quint16 registerValue = 0;
        // In-process link: the model applies the request immediately, and the
        // readback confirmation is carried to the terminal completion.
        bool confirmed = false;
    };

    void publishSnapshot();
    void setOnline(bool online);
    SubmissionResult accept(PlcOperation operation, quint16 address, bool coilValue,
                            quint16 registerValue, const QString &offlineReason);
    void completePending();
    void failPending(const QString &reason);

    SimulationClock m_clock;
    H3uSimulationModel m_model;

    bool m_started = false;
    bool m_online = false;
    bool m_linkDown = false;
    bool m_heartbeatFrozen = false;
    bool m_offlineDueToFreeze = false;

    quint64 m_tickSeconds = 1;
    quint64 m_sequence = 0;
    quint64 m_gatewayGeneration = 0;
    bool m_generationAssigned = false;
    quint64 m_nextRequestId = 1;
    quint64 m_reconnectCount = 0;
    quint64 m_failedPolls = 0;

    QVector<PendingSubmission> m_pending;

    // D140 heartbeat freeze tracking (spec §8.4): 3 ticks without a change.
    quint64 m_freezeTicks = 0;

    std::optional<DeviceSnapshot> m_lastSnapshot;
};

} // namespace hlm
