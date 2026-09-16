#include "adapters/simulator/simulated_plc_gateway.h"

#include <QDateTime>

namespace hlm {

namespace {

// Poll block definitions (spec §8.3), mirroring the real gateway's plan.
constexpr quint16 kFastStart = 100;    // D100
constexpr quint16 kFastCount = 41;     // D100-D140
constexpr quint16 kHomeStart = 50;     // M50
constexpr quint16 kHomeCount = 4;      // M50-M53
constexpr quint16 kCommandStart = 100; // M100
constexpr quint16 kCommandCount = 12;  // M100-M111 (M112 removed, D3)
constexpr quint16 kSlowStart = 204;    // D204
constexpr quint16 kSlowCount = 20;     // D204-D223

// D140 heartbeat freeze threshold (spec §8.4): 3 ticks without a change.
constexpr quint64 kHeartbeatFreezeTicks = 3;

// Pack M50-M53 into the home-bits word (bit0 = M50, ... bit3 = M53).
quint16 packHomeBits(const H3uSimulationModel &m)
{
    quint16 bits = 0;
    for (int i = 0; i < kHomeCount; ++i) {
        if (m.readCoil(kHomeStart + i))
            bits |= quint16(1) << i;
    }
    return bits;
}

// Pack M100-M111 into the command-bits word (bit0 = M100, ... bit11 = M111).
quint16 packCommandBits(const H3uSimulationModel &m)
{
    quint16 bits = 0;
    for (int i = 0; i < kCommandCount; ++i) {
        if (m.readCoil(kCommandStart + i))
            bits |= quint16(1) << i;
    }
    return bits;
}

} // namespace

SimulatedPlcGateway::SimulatedPlcGateway(QObject *parent)
    : IPlcGateway(parent)
    , m_model(m_clock)
{
    registerPlcGatewayMetaTypes();
}

SimulatedPlcGateway::~SimulatedPlcGateway() = default;

void SimulatedPlcGateway::start()
{
    if (m_started)
        return; // double start() must not reset the model mid-scenario
    m_started = true;
    m_linkDown = false;
    m_heartbeatFrozen = false;
    m_offlineDueToFreeze = false;
    m_sequence = 0;
    m_freezeTicks = 0;
    m_pending.clear();
    m_lastSnapshot.reset();
    if (!m_generationAssigned)
        ++m_gatewayGeneration;
    publishSnapshot();
    setOnline(true);
}

void SimulatedPlcGateway::stop()
{
    if (!m_started)
        return;
    m_started = false;
    // Accepted-but-unfinished submissions converge; nothing is silently
    // dropped (contract IPlcGateway lifecycle).
    failPending(QStringLiteral("gateway stopped"));
    m_lastSnapshot.reset();
    setOnline(false);
}

bool SimulatedPlcGateway::isOnline() const
{
    return m_online;
}

void SimulatedPlcGateway::setGatewayGeneration(quint64 generation)
{
    m_gatewayGeneration = generation;
    m_generationAssigned = true;
}

quint64 SimulatedPlcGateway::gatewayGeneration() const
{
    return m_gatewayGeneration;
}

SubmissionResult SimulatedPlcGateway::accept(PlcOperation operation, quint16 address,
                                             bool coilValue, quint16 registerValue,
                                             const QString &offlineReason)
{
    SubmissionResult r;
    r.gateway_generation = m_gatewayGeneration;
    if (!m_started || !m_online) {
        r.accepted = false;
        r.request_id = 0;
        r.immediate_rejection_reason = offlineReason;
        return r;
    }
    r.accepted = true;
    r.request_id = m_nextRequestId++;
    // The in-process link delivers the request immediately: apply it to the
    // model and capture the readback confirmation for the terminal completion
    // (which is emitted on the next tick).
    PendingSubmission pending{r.request_id, m_gatewayGeneration, operation,
                              address, coilValue, registerValue, false};
    switch (operation) {
    case PlcOperation::WriteCoil:
        m_model.writeCoil(address, coilValue);
        pending.confirmed = m_model.readCoil(address) == coilValue;
        break;
    case PlcOperation::WriteRegister:
        m_model.writeRegister(address, registerValue);
        pending.confirmed = m_model.readRegister(address) == registerValue;
        break;
    case PlcOperation::Pulse: {
        // Pulse (spec §8.5): serially write 1 then 0; the model reacts to the
        // rising edge (M101/M102/M103/M43).
        m_model.writeCoil(address, true);
        const bool setOk = m_model.readCoil(address);
        m_model.writeCoil(address, false);
        const bool clearOk = !m_model.readCoil(address);
        pending.confirmed = setOk && clearOk;
        break;
    }
    }
    m_pending.append(pending);
    return r;
}

SubmissionResult SimulatedPlcGateway::submitWriteCoil(quint16 address, bool value,
                                                      CommandPriority priority)
{
    Q_UNUSED(priority);
    return accept(PlcOperation::WriteCoil, address, value, 0,
                  QStringLiteral("offline: command rejected, not replayed"));
}

SubmissionResult SimulatedPlcGateway::submitWriteRegister(quint16 address, quint16 value,
                                                          CommandPriority priority)
{
    Q_UNUSED(priority);
    return accept(PlcOperation::WriteRegister, address, false, value,
                  QStringLiteral("offline: command rejected, not replayed"));
}

SubmissionResult SimulatedPlcGateway::submitPulse(quint16 address)
{
    return accept(PlcOperation::Pulse, address, false, 0,
                  QStringLiteral("offline: pulse rejected, not replayed"));
}

void SimulatedPlcGateway::tick()
{
    if (!m_started || m_linkDown)
        return; // link down: D140 freezes, no snapshots (spec §15.4)

    if (m_heartbeatFrozen) {
        // Dead PLC: the model clock does not advance, so D140 stays frozen.
        // 3 ticks without a change take the link offline (spec §8.4).
        ++m_freezeTicks;
        if (m_freezeTicks >= kHeartbeatFreezeTicks) {
            m_offlineDueToFreeze = true;
            setOnline(false);
            return; // no snapshot while offline
        }
        publishSnapshot();
        return;
    }

    const bool reconnecting = !m_online;
    m_model.advance(m_tickSeconds);
    if (m_offlineDueToFreeze) {
        // Heartbeat moving again: reconnect with a fresh snapshot.
        m_offlineDueToFreeze = false;
        m_freezeTicks = 0;
    }
    if (reconnecting) {
        ++m_gatewayGeneration; // post-reconnect events carry the new generation
        ++m_reconnectCount;    // comm stats (spec §16)
    }

    publishSnapshot();
    completePending();
    setOnline(true);
}

void SimulatedPlcGateway::setLinkDown(bool down)
{
    if (m_linkDown == down)
        return;
    m_linkDown = down;
    if (down) {
        // Accepted-but-unfinished submissions converge as communications lost
        // (never left pending, never replayed).
        failPending(QStringLiteral("communications lost: command not replayed"));
        setOnline(false);
    } else {
        // Restored: still offline until the next tick reconnects with a
        // fresh snapshot (mirrors the real gateway's reconnect rule). The
        // reconnect counter increments when that reconnect actually happens.
        m_freezeTicks = 0;
    }
}

void SimulatedPlcGateway::setHeartbeatFrozen(bool frozen)
{
    m_heartbeatFrozen = frozen;
    if (!frozen) {
        // Unfrozen: reset the freeze counter unconditionally so a later
        // re-freeze starts from a clean slate (spec §8.4).
        m_freezeTicks = 0;
    }
}

void SimulatedPlcGateway::publishSnapshot()
{
    // Fast block D100-D140 (spec §8.3): reuse the centralized decoder. The
    // model maintains the D100/D103 status words itself (single source of
    // truth, spec §8.2), so the fast-block registers already carry the
    // M0-M14 / M30-M45 mapping — no synthesis here (or the two paths diverge).
    quint16 raw[kFastCount] = {};
    for (int i = 0; i < kFastCount; ++i)
        raw[i] = m_model.readRegister(kFastStart + i);

    const QDateTime now = QDateTime::currentDateTime();
    DeviceSnapshotData d = decodeFastBlock(raw, ++m_sequence, true, 0, now, now,
                                           DataQuality::Valid);

    // Home bits M50-M53 and command bits M100-M111 (function code 01).
    d.homeBits = packHomeBits(m_model);
    d.commandBits = packCommandBits(m_model);
    // The in-process model completes every poll inline, so each block was
    // refreshed now: real zero ages and Valid quality (no hard-coded overall
    // age; recomputeDerivedQuality derives the maximum).
    d.fast_age_ms = 0;
    d.home_age_ms = 0;
    d.home_quality = DataQuality::Valid;
    d.command_age_ms = 0;
    d.command_quality = DataQuality::Valid;
    d.slow_age_ms = 0;
    d.slow_quality = DataQuality::Valid;

    // Slow block D204-D223 (spec §8.3): D204 -> 0, D210 -> 6, D220 -> 16.
    d.pulsePerMm = m_model.readRegister(kSlowStart);
    d.widthDelta = decode::i16(m_model.readRegister(kSlowStart + 6));
    d.widthSpeed = m_model.readRegister(kSlowStart + 16);
    // Out-of-range D204/D220 mark the field invalid (spec §9) and D210 gets
    // its own validity metadata.
    checkSlowBlockRange(d);
    recomputeDerivedQuality(d);

    m_lastSnapshot = DeviceSnapshot(d);
    emit snapshotReady(m_gatewayGeneration, m_lastSnapshot.value());

    // Communication statistics ride along with every published snapshot
    // (spec §16, contract event_fields): the counters belong to this snapshot
    // and are emitted by the port itself.
    PlcCommStats stats;
    stats.gateway_generation = m_gatewayGeneration;
    stats.snapshot_sequence = m_sequence;
    stats.per_block_age_ms = m_lastSnapshot->overall_age_ms;
    stats.reconnect_count = m_reconnectCount;
    stats.failed_polls = m_failedPolls;
    emit commStatsChanged(stats);
}

void SimulatedPlcGateway::completePending()
{
    const QVector<PendingSubmission> pending = m_pending;
    m_pending.clear();
    for (const PendingSubmission &p : pending) {
        SubmissionCompletion c;
        c.request_id = p.request_id;
        c.gateway_generation = p.gateway_generation;
        c.operation = p.operation;
        c.address = p.address;
        c.result = p.confirmed;
        if (!p.confirmed) {
            c.error = p.operation == PlcOperation::Pulse
                ? QStringLiteral("pulse not confirmed by readback")
                : QStringLiteral("write not confirmed by readback");
        }
        emit submissionCompleted(c);
    }
}

void SimulatedPlcGateway::failPending(const QString &reason)
{
    const QVector<PendingSubmission> pending = m_pending;
    m_pending.clear();
    for (const PendingSubmission &p : pending) {
        SubmissionCompletion c;
        c.request_id = p.request_id;
        c.gateway_generation = p.gateway_generation;
        c.operation = p.operation;
        c.address = p.address;
        c.result = false;
        c.error = reason;
        emit submissionCompleted(c);
    }
}

void SimulatedPlcGateway::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    emit connectionStateChanged(m_gatewayGeneration, online);
}

} // namespace hlm
