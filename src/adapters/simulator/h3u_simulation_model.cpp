#include "adapters/simulator/h3u_simulation_model.h"

#include <QtGlobal>

namespace hlm {

namespace {

// Protocol addresses (0-based, matching AddressTable).
constexpr quint16 kM0 = 0;   // estop active
constexpr quint16 kM1 = 1;   // manual mode
constexpr quint16 kM2 = 2;   // auto mode
constexpr quint16 kM3 = 3;   // running
constexpr quint16 kM14 = 14; // latched fault
constexpr quint16 kM34 = 34; // width adjusting
constexpr quint16 kM43 = 43; // width adjust command (pulse)
constexpr quint16 kM44 = 44; // width adjust success
constexpr quint16 kM45 = 45; // width adjust failure
constexpr quint16 kM50 = 50; // homing
constexpr quint16 kM60 = 60; // auto ready
constexpr quint16 kM61 = 61; // homed
constexpr quint16 kM100 = 100; // HMI estop request
constexpr quint16 kM101 = 101; // HMI start
constexpr quint16 kM102 = 102; // HMI stop
constexpr quint16 kM103 = 103; // HMI reset
constexpr quint16 kM104 = 104; // auto mode select
constexpr quint16 kM105 = 105; // passthrough mode
constexpr quint16 kM106 = 106; // manual width forward
constexpr quint16 kM107 = 107; // manual width reverse
constexpr quint16 kM108 = 108; // manual belt jog
constexpr quint16 kM109 = 109; // manual stop gate
constexpr quint16 kM110 = 110; // light curtain bypass
constexpr quint16 kM111 = 111; // door bypass
constexpr quint16 kM112 = 112; // HMI watchdog

constexpr quint16 kD110 = 110; // fault code
constexpr quint16 kD126 = 126; // width frequency low word
constexpr quint16 kD127 = 127; // width frequency high word
constexpr quint16 kD128 = 128; // target width
constexpr quint16 kD130 = 130; // current width
constexpr quint16 kD136 = 136; // pulse count low word
constexpr quint16 kD137 = 137; // pulse count high word
constexpr quint16 kD138 = 138; // production count low word
constexpr quint16 kD139 = 139; // production count high word
constexpr quint16 kD140 = 140; // heartbeat
constexpr quint16 kD204 = 204; // pulse per mm
constexpr quint16 kD210 = 210; // target - current (signed 16-bit)
constexpr quint16 kD212 = 212; // latched target of the current command
constexpr quint16 kD213 = 213; // |target - current|
constexpr quint16 kD214 = 214; // DIV numerator
constexpr quint16 kD216 = 216; // DIV quotient
constexpr quint16 kD218 = 218; // dynamic timeout seconds (10-360)
constexpr quint16 kD220 = 220; // width speed (mm/s)
constexpr quint16 kD222 = 222; // T6 preset in 100 ms units

constexpr quint16 kHomeReturnSeconds = 2;
constexpr quint16 kWatchdogSeconds = 2;

} // namespace

H3uSimulationModel::H3uSimulationModel(SimulationClock &clock)
    : m_clock(clock)
{
    // Defaults (spec §10.3.1): manual mode, current width 200, target 200,
    // pulse per mm 1280, width speed 15 mm/s. D128 == D130 so an M43 command
    // is invalid until the HMI writes a real target.
    m_coils[kM1] = true; // manual mode
    m_regs[kD130] = 200;
    m_regs[kD128] = 200;
    m_regs[kD204] = 1280;
    m_regs[kD220] = 15;
    updateD126();
    updateD210();
}

void H3uSimulationModel::writeCoil(quint16 addr, bool value)
{
    if (addr > 112)
        return;
    const bool rising = value && !m_coils[addr];
    m_coils[addr] = value;

    switch (addr) {
    case kM43: // width adjust command (pulse)
        if (rising)
            onM43RisingEdge();
        break;
    case kM103:
        if (rising)
            onM103RisingEdge();
        break;
    case kM104:
        onM104Write(value);
        break;
    case kM101:
        if (rising)
            onM101RisingEdge();
        break;
    case kM102:
        if (rising)
            onM102RisingEdge();
        break;
    case kM100:
        onM100Write(value);
        break;
    case kM112:
        onM112Write(value);
        break;
    default:
        break;
    }
}

bool H3uSimulationModel::readCoil(quint16 addr) const
{
    return addr <= 112 && m_coils[addr];
}

void H3uSimulationModel::writeRegister(quint16 addr, quint16 value)
{
    if (addr > 223)
        return;
    m_regs[addr] = value;
    if (addr == kD128 || addr == kD130)
        updateD210();
    if (addr == kD204 || addr == kD220)
        updateD126();
}

quint16 H3uSimulationModel::readRegister(quint16 addr) const
{
    return addr <= 223 ? m_regs[addr] : 0;
}

quint32 H3uSimulationModel::readRegister32(quint16 lowAddr) const
{
    if (lowAddr + 1 > 223)
        return 0;
    return quint32(m_regs[lowAddr]) | (quint32(m_regs[lowAddr + 1]) << 16);
}

void H3uSimulationModel::setProductionCount(quint32 count)
{
    m_regs[kD138] = quint16(count & 0xFFFF);
    m_regs[kD139] = quint16(count >> 16);
}

void H3uSimulationModel::setHomeReturnFault(int code)
{
    m_homeFault = code;
}

void H3uSimulationModel::setPositioningStall(bool stall)
{
    m_stall = stall;
}

void H3uSimulationModel::advance(quint64 seconds)
{
    m_clock.advance(seconds);
    tick(seconds);
}

// --- M43 rising edge: width adjust command (spec §10.3.1) -------------------

void H3uSimulationModel::onM43RisingEdge()
{
    // Every new command first clears the old results and the old timer.
    m_coils[kM44] = false;
    m_coils[kM45] = false;
    m_t6Elapsed = 0;

    const bool busy = m_coils[kM34];
    updateM49();
    const bool preconditions = m_coils[kM1] && m_coils[kM61] && !m_coils[kM3]
        && !m_coils[kM0] && !m_coils[kM14] && !m_coils[kM50] && m_m49
        && m_regs[kD128] >= 50 && m_regs[kD128] <= 400
        && m_regs[kD204] >= 1 && m_regs[kD204] <= 32767
        && m_regs[kD220] >= 1 && m_regs[kD220] <= 15
        && readRegister32(kD126) >= 10 && readRegister32(kD126) <= 200000
        && m_regs[kD128] != m_regs[kD130];

    if (busy) {
        // Width adjust while already adjusting: safe abort, no second run.
        m_coils[kM45] = true;
        m_coils[kM34] = false;
        m_positioning = false;
        m_remaining = 0;
        m_t6Elapsed = 0;
        return;
    }

    if (!preconditions) {
        // Not busy but preconditions invalid: only M45.
        m_coils[kM45] = true;
        m_coils[kM34] = false;
        return;
    }

    // Valid command: latch the target and compute the difference.
    m_regs[kD212] = m_regs[kD128];
    updateD210(); // D210 = D212 - D130 (signed 16-bit)
    m_regs[kD213] = quint16(qAbs(qint16(m_regs[kD210])));

    // ceil(abs(diff) / speed) + 5 seconds (spec §10.3.1).
    const quint16 speed = m_regs[kD220] > 0 ? m_regs[kD220] : 1;
    const quint16 distance = m_regs[kD213];
    m_regs[kD214] = quint16(distance + speed - 1); // numerator for ceil
    m_regs[kD216] = quint16(m_regs[kD214] / speed);
    quint32 timeout = quint32(m_regs[kD216]) + 5;
    if (timeout < 10)
        timeout = 10;
    if (timeout > 360)
        timeout = 360;
    m_regs[kD218] = quint16(timeout);
    m_regs[kD222] = quint16(timeout * 10); // T6 preset, 100 ms units

    // Pulse count = signed 16x16 MUL -> 32-bit, low word first.
    const qint32 pulses = qint32(qint16(m_regs[kD210])) * qint32(qint16(m_regs[kD204]));
    m_regs[kD136] = quint16(quint32(pulses) & 0xFFFF);
    m_regs[kD137] = quint16(quint32(pulses) >> 16);

    m_coils[kM34] = true;
    m_positioning = true;
    m_remaining = m_regs[kD216]; // positioning duration = ceil(diff / speed)
    m_t6Elapsed = 0;
}

// --- M103 rising edge: reset + home return (spec §10.2, §10.3.1) ------------

void H3uSimulationModel::onM103RisingEdge()
{
    // Reset command synchronously clears the width-adjust state.
    m_coils[kM34] = false;
    m_coils[kM44] = false;
    m_coils[kM45] = false;
    m_positioning = false;
    m_remaining = 0;
    m_t6Elapsed = 0;

    // M103 also clears a latched fault (spec §10.6: only M103 resets it).
    m_coils[kM14] = false;
    m_regs[kD110] = 0;

    // Start home return.
    m_coils[kM50] = true;
    m_coils[kM61] = false;
    m_homeRemaining = kHomeReturnSeconds;
}

// --- M104: mode select (spec §10.1) -----------------------------------------

void H3uSimulationModel::onM104Write(bool value)
{
    if (m_coils[kM3]) {
        // Running: mode switch rejected (spec §10.1).
        return;
    }
    m_coils[kM2] = value;
    m_coils[kM1] = !value;
}

// --- M101: start (spec §10.4) -----------------------------------------------

void H3uSimulationModel::onM101RisingEdge()
{
    if (m_coils[kM2] && m_coils[kM60] && !m_coils[kM0] && !m_coils[kM14]
        && !m_coils[kM3]) {
        m_coils[kM3] = true;
    }
}

// --- M102: stop (spec §10.5) ------------------------------------------------

void H3uSimulationModel::onM102RisingEdge()
{
    m_coils[kM3] = false;
}

// --- M100: software estop (spec §10.6) --------------------------------------

void H3uSimulationModel::onM100Write(bool value)
{
    if (value) {
        m_coils[kM0] = true;
        m_coils[kM14] = true;
        m_regs[kD110] = 1;
        m_coils[kM3] = false;
    } else {
        // Release clears M0 only; the fault stays latched until M103.
        m_coils[kM0] = false;
    }
}

// --- M112: HMI watchdog (spec §8.6) -----------------------------------------

void H3uSimulationModel::onM112Write(bool value)
{
    if (value) {
        m_watchdogElapsed = 0;
        m_watchdogArmed = true;
    }
}

// --- continuous interlock M49 (spec §10.3.1) --------------------------------

void H3uSimulationModel::updateM49()
{
    m_m49 = m_coils[kM1] && m_coils[kM61] && !m_coils[kM3] && !m_coils[kM0]
        && !m_coils[kM14] && !m_coils[kM50];
}

// --- derived state -----------------------------------------------------------

void H3uSimulationModel::updateM60()
{
    m_coils[kM60] = m_coils[kM61] && m_regs[kD130] >= 50 && m_regs[kD130] <= 400;
}

void H3uSimulationModel::updateD210()
{
    // D210 = D128 - D130 (signed 16-bit): the live target minus the current
    // width, maintained continuously (spec §10.3.1 "D210 始终表示实时目标减当前").
    m_regs[kD210] = quint16(qint16(m_regs[kD128]) - qint16(m_regs[kD130]));
}

void H3uSimulationModel::updateD126()
{
    // D126/D127 = D220 * D204 (32-bit frequency, low word first).
    const quint32 freq = quint32(m_regs[kD220]) * quint32(m_regs[kD204]);
    m_regs[kD126] = quint16(freq & 0xFFFF);
    m_regs[kD127] = quint16(freq >> 16);
}

// --- time tick ---------------------------------------------------------------

void H3uSimulationModel::tick(quint64 seconds)
{
    // D140 heartbeat: increments every second, 16-bit wrap.
    m_regs[kD140] = quint16(m_regs[kD140] + seconds);

    // Home return.
    if (m_coils[kM50]) {
        if (seconds >= m_homeRemaining) {
            m_coils[kM50] = false;
            m_homeRemaining = 0;
            if (m_homeFault != 0) {
                m_coils[kM61] = false;
                m_coils[kM14] = true;
                m_regs[kD110] = quint16(m_homeFault);
            } else {
                m_coils[kM61] = true;
            }
            updateM60();
        } else {
            m_homeRemaining -= seconds;
        }
    }

    // Width adjustment progress and dynamic timeout (T6, 100 ms units).
    if (m_positioning) {
        m_t6Elapsed += seconds * 10;
        const bool timedOut = m_t6Elapsed >= quint64(m_regs[kD222]);
        if (timedOut) {
            // Dynamic timeout (spec §10.3.1): M45, M14, D110 = 10.
            m_coils[kM34] = false;
            m_coils[kM44] = false;
            m_coils[kM45] = true;
            m_coils[kM14] = true;
            m_regs[kD110] = 10;
            m_positioning = false;
            m_remaining = 0;
            m_t6Elapsed = 0;
        } else if (!m_stall && seconds >= m_remaining) {
            // Normal completion (spec §10.3.1): D130 = latched target.
            m_coils[kM34] = false;
            m_regs[kD130] = m_regs[kD212];
            updateD210(); // D210 = D128 - D130
            m_coils[kM45] = false;
            m_coils[kM44] = true;
            m_positioning = false;
            m_remaining = 0;
            m_t6Elapsed = 0;
        } else if (!m_stall) {
            m_remaining -= seconds;
        }
    }

    // M112 watchdog: 2 s without an edge clears M42/M106-M111 (spec §8.6).
    if (m_watchdogArmed) {
        m_watchdogElapsed += seconds;
        if (m_watchdogElapsed >= kWatchdogSeconds) {
            m_coils[42] = false;
            for (quint16 a = kM106; a <= kM111; ++a)
                m_coils[a] = false;
            m_watchdogArmed = false;
            m_watchdogElapsed = 0;
        }
    }
}

} // namespace hlm
