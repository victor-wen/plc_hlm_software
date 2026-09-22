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
constexpr quint16 kM15 = 15; // 拍照结束/扫码结束 (scan cycle end)
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

// Highest coil the model accepts. M112 stays an unused hole (PLC-HMI-003 D3);
// the range extends to M117 for the 测试信号 pulses (user decision 2026-09-22),
// which the PLC program does not consume yet.
constexpr quint16 kLastCoilAddress = 117;

constexpr quint16 kD100 = 100; // status word 1: M0-M14 map, bit15 reserved
constexpr quint16 kD103 = 103; // status word 3: M30-M45 map
constexpr quint16 kD110 = 110; // fault code
constexpr quint16 kD122 = 122; // belt speed
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
constexpr quint16 kD220 = 220; // width speed (mm/s)

constexpr quint16 kHomeReturnSeconds = 2;

// PLC-authoritative parity constants (PLC-HMI-005 D1/D2): the decoded PLC
// derives D126/D127 from a fixed K1280 factor (never D204) and uses a fixed
// T6 K300 width timeout independent of the pulse configuration.
constexpr quint32 kFixedFrequencyFactor = 1280;
constexpr quint16 kD220Max = 15;
constexpr quint64 kFixedTimeoutTicks = 300; // T6 K300: 300 * 100 ms = 30 s

} // namespace

H3uSimulationModel::H3uSimulationModel(SimulationClock &clock)
    : m_clock(clock)
{
    // PLC-authoritative defaults (PLC-HMI-005 D1): manual mode, current width
    // 200, target 200, belt speed 5000, pulse per mm 128, width speed
    // initialized to 20 and clamped to the effective 15. D128 == D130 so an
    // M43 command is invalid until the HMI writes a real target.
    m_coils[kM1] = true; // manual mode
    m_regs[kD130] = 200;
    m_regs[kD128] = 200;
    m_regs[kD122] = 5000;
    m_regs[kD204] = 128;
    m_regs[kD220] = 20; // clamped to 15 by clampD220()
    clampD220();
    updateD210();
}

void H3uSimulationModel::writeCoil(quint16 addr, bool value)
{
    if (addr > kLastCoilAddress)
        return;
    const bool rising = value && !m_coils[addr];
    m_coils[addr] = value;

    switch (addr) {
    case kM15: // 拍照结束/扫码结束 (test injection, user decision 2026-09-22)
        // The in-process pulse writes 1 then 0 inside one call, so the level is
        // never visible in a snapshot. Latch the rising edge; the in-process
        // gateway consumes it for exactly one snapshot, which is what the HMI's
        // rising-edge detection needs. Over Modbus RTU the HMI's own 100 ms
        // level is what the PLC sees, so the latch is not used on that path.
        if (rising)
            m_scanCompletePulse = true;
        break;
    case kM43: // width adjust command (pulse)
        if (rising)
            onM43RisingEdge();
        break;
    case kM50:
        if (rising) {
            // HMI home-start request (PLC-HMI-011 D6): a single sustained M50=1
            // write starts the home return and clears home-complete. M50 is
            // cleared by tick() itself when homing finishes (PLC-owned clear).
            m_coils[kM61] = false;
            m_homeRemaining = kHomeReturnSeconds;
        }
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
    default:
        break;
    }
}

bool H3uSimulationModel::takeScanCompletePulse()
{
    const bool pulse = m_scanCompletePulse;
    m_scanCompletePulse = false;
    return pulse;
}

bool H3uSimulationModel::readCoil(quint16 addr) const
{
    return addr <= kLastCoilAddress && m_coils[addr];
}

void H3uSimulationModel::writeRegister(quint16 addr, quint16 value)
{
    if (addr > 223)
        return;
    // D100/D103 are read-only PLC status words synthesized from the coils
    // (spec §8.2, address table Read): a raw register write must not
    // desynchronize them from the M-bit state.
    if (addr == kD100 || addr == kD103)
        return;
    m_regs[addr] = value;
    if (addr == kD128 || addr == kD130) {
        updateD210();
        // Same per-scan derived state tick() evaluates: reaching the target
        // resets T6, and M60 is a function of D128/D130. Kept consistent here
        // so a caller that reads the derived coils before the next tick never
        // sees a stale M60/T6 pair.
        if (m_regs[kD128] == m_regs[kD130])
            m_t6Done = false;
        updateM60();
    }
    if (addr == kD220)
        clampD220(); // D126/D127 = D220 * fixed K1280
}

quint16 H3uSimulationModel::readRegister(quint16 addr) const
{
    // D100/D103 are derived, so they are always consistent with the current
    // coil state (no update-timing window for the external RTU server).
    if (addr == kD100)
        return statusWord1();
    if (addr == kD103)
        return statusWord3();
    return addr <= 223 ? m_regs[addr] : 0;
}

quint16 H3uSimulationModel::statusWord1() const
{
    // spec §8.2: D100 bit0-14 expose M0-M14; M60/M61 are mapped onto the
    // M8/M9 positions (bit8/bit9). Bit15 is reserved and always 0.
    //
    // bit8 = M8 | M60 and bit9 = M9 | M61. Coils 8/9 are never used by this
    // model, so in practice bit8/bit9 == M60/M61; the union is deliberately
    // kept for byte-for-byte parity with the behavior the in-process gateway
    // synthesized before it was unified here. Do NOT change it to M60/M61 only.
    quint16 word = 0;
    for (int bit = 0; bit <= 14; ++bit) {
        if (m_coils[bit])
            word |= quint16(1) << bit;
    }
    if (m_coils[kM60])
        word |= quint16(1) << 8; // M8 = M60 (auto ready)
    if (m_coils[kM61])
        word |= quint16(1) << 9; // M9 = M61 (homed)
    return word;
}

quint16 H3uSimulationModel::statusWord3() const
{
    // spec §8.2: D103 bit0-15 expose M30-M45.
    quint16 word = 0;
    for (int bit = 0; bit <= 15; ++bit) {
        if (m_coils[30 + bit])
            word |= quint16(1) << bit;
    }
    return word;
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

void H3uSimulationModel::setEstopReleaseStuck(bool stuck)
{
    m_estopReleaseStuck = stuck;
    // Re-assert M0 to reflect the physical estop keeping the machine stopped.
    if (stuck)
        m_coils[kM0] = true;
}

void H3uSimulationModel::abortWidthAdjust()
{
    if (!m_positioning && !m_coils[kM34])
        return;
    // Safe abort: keep the existing M45 failure indication without touching
    // the latched fault code (an estop fault must not be overwritten).
    m_coils[kM34] = false;
    m_coils[kM44] = false;
    m_coils[kM45] = true;
    m_t6Elapsed = 0;
    m_positioning = false;
    m_remaining = 0;
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
    // Decoded PLC start preconditions (PLC-HMI-005 D2): no M49 occupancy and
    // no D128/D204/D220/D126 range gating beyond the fields' own decodes.
    // 50-400 mm is the intentional operator/recipe envelope (需求 D128 50~400,
    // PLC-HMI-006 D5), enforced by the HMI interlock only: the parity model
    // intentionally models the decoded rungs and keeps no width envelope.
    const bool preconditions = m_coils[kM1] && m_coils[kM61] && !m_coils[kM3]
        && !m_coils[kM0] && !m_coils[kM14] && !m_coils[kM50]
        && m_regs[kD128] != m_regs[kD130];

    if (busy) {
        abortWidthAdjust();
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

    // Pulse count = signed 16x16 MUL -> 32-bit, low word first.
    const qint32 pulses = qint32(qint16(m_regs[kD210])) * qint32(qint16(m_regs[kD204]));
    m_regs[kD136] = quint16(quint32(pulses) & 0xFFFF);
    m_regs[kD137] = quint16(quint32(pulses) >> 16);

    // Ideal motion duration = pulse load / drive frequency:
    // ceil(|D210| * D204 / (D220 * fixed K1280)) seconds. The 30 s T6 K300
    // timeout is fixed and independent of this duration (PLC-HMI-005 D2).
    const quint64 frequency = quint64(m_regs[kD220]) * kFixedFrequencyFactor;
    if (frequency > 0) {
        const quint64 pulseLoad = quint64(qAbs(qint32(qint16(m_regs[kD210]))))
            * quint64(m_regs[kD204]);
        m_remaining = (pulseLoad + frequency - 1) / frequency;
    } else {
        // D220 = 0 (outside the decoded 1-15 range, unreachable from the HMI:
        // spin range + paramReasons + interlock + clamp all enforce it) makes
        // the ideal duration exceed the fixed 30 s T6 K300 window. tick()
        // evaluates completion before the timeout in the same scan, so such a
        // run completes successfully instead of timing out: intentional parity
        // with the decoded PLC, and no in-contract path reaches this branch.
        m_remaining = kFixedTimeoutTicks / 10; // D220 = 0: timeout-bound
    }

    m_coils[kM34] = true;
    m_positioning = true;
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

    // PLC-HMI-011 D6: the reset pulse no longer starts homing. Homing starts
    // only from the HMI's sustained M50=1 write; M103 still clears the
    // home-complete bit (spec §10.2). M60 is an OUT coil driven by M61, so the
    // reset clears 自动准备完成 with it (user decision 2026-09-22); it is
    // recomputed here so the clear is visible immediately rather than only on
    // the next tick.
    m_coils[kM61] = false;
    updateM60();
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
        // Abort any in-flight positioning run without overwriting the fault
        // code just latched above (spec §10.3.1).
        abortWidthAdjust();
    } else {
        // Release clears M0 only; the fault stays latched until M103.
        // A stuck physical estop keeps M0=1 despite the M100=0 release write.
        if (!m_estopReleaseStuck)
            m_coils[kM0] = false;
    }
}

// --- derived state -----------------------------------------------------------

void H3uSimulationModel::updateM60()
{
    // Decoded rung (SBR_FAULT.LD, rung comment "自动准备完成：回原点完成 +
    // 调宽到位 + 无故障"), confirmed element-by-element against the ladder:
    // M60 = M61 AND (D128 == D130) AND NOT M0 AND NOT M14 AND NOT T6,
    // re-evaluated as an OUT coil every scan. The former "D130 in 50..400"
    // term was an HMI invention from the pre-decode spec (commit a337510): no
    // ladder rung compares D130 against a constant, and with the 0.1 mm width
    // units (user decision 2026-09-21) it would have meant a 5.0-40.0 mm
    // window. PLC-HMI-005 D2 removed the sibling width gates from the M43
    // preconditions for the same reason.
    //
    // ¬T6 (user decision 2026-09-22): T6 is the `T6 K300` width-adjust timeout
    // in SBR_MANUALWIDTH, reset only by `LD= D128 D130` → `RST T6`. A width
    // timeout therefore keeps M60 at 0 until a later run actually reaches the
    // target — clearing the fault with M103 is not enough on its own.
    m_coils[kM60] = m_coils[kM61] && m_regs[kD128] == m_regs[kD130]
        && !m_coils[kM0] && !m_coils[kM14] && !m_t6Done;
}

void H3uSimulationModel::updateD210()
{
    // D210 = D128 - D130 (signed 16-bit): the live target minus the current
    // width, maintained continuously (spec §10.3.1 "D210 始终表示实时目标减当前").
    m_regs[kD210] = quint16(qint16(m_regs[kD128]) - qint16(m_regs[kD130]));
}

void H3uSimulationModel::updateD126()
{
    // D126/D127 = D220 * fixed K1280 (32-bit frequency, low word first).
    // D204 is never part of this formula (PLC-HMI-005 D1).
    const quint32 freq = quint32(m_regs[kD220]) * kFixedFrequencyFactor;
    m_regs[kD126] = quint16(freq & 0xFFFF);
    m_regs[kD127] = quint16(freq >> 16);
}

void H3uSimulationModel::clampD220()
{
    // The decoded PLC powers up at 20 and exposes the effective clamp of 15
    // (PLC-HMI-005 D1); the same ceiling applies to raw register writes.
    if (m_regs[kD220] > kD220Max)
        m_regs[kD220] = kD220Max;
    updateD126();
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
                // SBR_HOME.LD zeroes the current width in the home-return
                // completion sequence (`DMOV K0 D130`, beside SET M52/RST M53):
                // the belt is back at the zero position. D128 != D130 after
                // homing, so M60 stays 0 until a later width adjust actually
                // reaches the target (user decision 2026-09-22).
                m_regs[kD130] = 0;
                updateD210();
            }
            updateM60();
        } else {
            m_homeRemaining -= seconds;
        }
    }

    // Width adjustment progress and the fixed T6 K300 (30 s) timeout.
    if (m_positioning) {
        m_t6Elapsed += seconds * 10;
        if (!m_stall && seconds >= m_remaining) {
            // Normal completion (spec §10.3.1): D130 = D128. The decoded rung
            // is `M8029 → DMOV D128 D130` — the LIVE target register, not a
            // latched copy (D212/D213 are spec-reference registers that the
            // supplied ladder does not contain). Checked before the timeout so
            // completion wins in the same scan.
            m_coils[kM34] = false;
            m_regs[kD130] = m_regs[kD128];
            updateD210(); // D210 = D128 - D130
            m_coils[kM45] = false;
            m_coils[kM44] = true;
            m_positioning = false;
            m_remaining = 0;
            m_t6Elapsed = 0;
        } else if (m_t6Elapsed >= kFixedTimeoutTicks) {
            // Fixed 30 s timeout (spec §10.3.1 T6 K300): M45, M14, D110 = 10.
            // T6 stays done (¬T6) until D128 == D130 resets it below.
            m_coils[kM34] = false;
            m_coils[kM44] = false;
            m_coils[kM45] = true;
            m_coils[kM14] = true;
            m_regs[kD110] = 10;
            m_t6Done = true;
            m_positioning = false;
            m_remaining = 0;
            m_t6Elapsed = 0;
        } else if (!m_stall) {
            m_remaining -= seconds;
        }
    }

    // Decoded SBR_MANUALWIDTH rung `LD= D128 D130` → `RST T6`: T6 is reset the
    // moment the current width reaches the target. SBR_MANUALWIDTH is called
    // before SBR_FAULT in MAIN.LD, so in the completion scan the reset lands
    // before M60 is recomputed and M60 rises in the same scan.
    if (m_regs[kD128] == m_regs[kD130])
        m_t6Done = false;

    // M60 is an OUT coil in the decoded ladder, so it tracks its terms on every
    // scan rather than only at the events that change them.
    updateM60();
}

} // namespace hlm
