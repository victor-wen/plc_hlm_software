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
constexpr quint16 kCommandCount = 13;  // M100-M112
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

// Pack M100-M112 into the command-bits word (bit0 = M100, ... bit12 = M112).
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
    m_lastSnapshot.reset();
    publishSnapshot();
    setOnline(true);
}

void SimulatedPlcGateway::stop()
{
    if (!m_started)
        return;
    m_started = false;
    m_lastSnapshot.reset();
    setOnline(false);
}

bool SimulatedPlcGateway::isOnline() const
{
    return m_online;
}

void SimulatedPlcGateway::writeCoil(quint16 address, bool value, CommandPriority priority)
{
    Q_UNUSED(priority);
    if (!m_online) {
        // Offline: rejected, never queued or replayed (spec §8.4).
        emit writeCompleted(address, false,
                            QStringLiteral("offline: command rejected, not replayed"));
        return;
    }
    m_model.writeCoil(address, value);
    // Write-then-readback confirmation (spec §8.4): the model ignores writes
    // outside its address space, so the readback cannot confirm them.
    const bool confirmed = m_model.readCoil(address) == value;
    emit writeCompleted(address, confirmed,
                        confirmed ? QString()
                                  : QStringLiteral("write not confirmed by readback"));
}

void SimulatedPlcGateway::writeRegister(quint16 address, quint16 value,
                                        CommandPriority priority)
{
    Q_UNUSED(priority);
    if (!m_online) {
        emit writeCompleted(address, false,
                            QStringLiteral("offline: command rejected, not replayed"));
        return;
    }
    m_model.writeRegister(address, value);
    const bool confirmed = m_model.readRegister(address) == value;
    emit writeCompleted(address, confirmed,
                        confirmed ? QString()
                                  : QStringLiteral("write not confirmed by readback"));
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

    m_model.advance(m_tickSeconds);

    if (m_offlineDueToFreeze) {
        // Heartbeat moving again: reconnect with a fresh snapshot.
        m_offlineDueToFreeze = false;
        m_freezeTicks = 0;
        publishSnapshot();
        setOnline(true);
        return;
    }

    publishSnapshot();
    setOnline(true);
}

void SimulatedPlcGateway::setLinkDown(bool down)
{
    if (m_linkDown == down)
        return;
    m_linkDown = down;
    if (down) {
        setOnline(false);
    } else {
        // Restored: still offline until the next tick reconnects with a
        // fresh snapshot (mirrors the real gateway's reconnect rule).
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
    // Fast block D100-D140 (spec §8.3): reuse the centralized decoder.
    quint16 raw[kFastCount] = {};
    for (int i = 0; i < kFastCount; ++i)
        raw[i] = m_model.readRegister(kFastStart + i);

    // The model stores coils directly; the real PLC ladder maps M0-M14 into
    // D100 and M30-M45 into D103 (spec §8.2). Synthesize the status words so
    // the snapshot exposes the same boolean states as the real gateway.
    for (int bit = 0; bit <= 14; ++bit) {
        if (m_model.readCoil(bit))
            raw[0] |= quint16(1) << bit; // D100
    }
    for (int bit = 0; bit <= 15; ++bit) {
        if (m_model.readCoil(30 + bit))
            raw[3] |= quint16(1) << bit; // D103
    }

    const QDateTime now = QDateTime::currentDateTime();
    DeviceSnapshotData d = decodeFastBlock(raw, ++m_sequence, true, 0, now, now,
                                           DataQuality::Valid);

    // Home bits M50-M53 and command bits M100-M112 (function code 01).
    d.homeBits = packHomeBits(m_model);
    d.commandBits = packCommandBits(m_model);

    // Slow block D204-D223 (spec §8.3): D204 -> 0, D210 -> 6, D220 -> 16.
    d.pulsePerMm = m_model.readRegister(kSlowStart);
    d.widthDelta = decode::i16(m_model.readRegister(kSlowStart + 6));
    d.widthSpeed = m_model.readRegister(kSlowStart + 16);

    d.overallQuality = aggregateQuality(d);
    m_lastSnapshot = DeviceSnapshot(d);
    emit snapshotReady(m_lastSnapshot.value());
}

void SimulatedPlcGateway::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    emit connectionStateChanged(online);
}

} // namespace hlm
