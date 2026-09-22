#pragma once

#include <QtGlobal>

#include "adapters/simulator/simulation_clock.h"

namespace hlm {

// Shared state model of the H3U PLC (spec §14.1). Simulates the control
// behavior of spec §10.2-§10.6 and the corrected reference ladder of
// §10.3.1: home return, width adjustment (M43/M34/M44/M45), the fixed T6
// K300 (30 s) width timeout, mode switching, start/stop, software estop and
// the D140 heartbeat. Time is injected via SimulationClock and advanced
// explicitly, so all scenarios run deterministically.
//
// The model is a plain class (no QObject): it only holds state and reacts
// to writes/advance calls. Both the in-process SimulatedPlcGateway (Task 6)
// and the standalone RTU simulator (Task 18) wrap this same model.
class H3uSimulationModel
{
public:
    explicit H3uSimulationModel(SimulationClock &clock);

    // --- coil / register access (0-based protocol addresses) ----------------
    void writeCoil(quint16 addr, bool value);
    bool readCoil(quint16 addr) const;
    void writeRegister(quint16 addr, quint16 value);
    quint16 readRegister(quint16 addr) const;
    // 32-bit read, low word first (D126/D127, D136/D137, D138/D139).
    quint32 readRegister32(quint16 lowAddr) const;

    // --- fault-injection hooks (used by the simulator control panels) --------
    void setProductionCount(quint32 count);
    void setHomeReturnFault(int code); // 0 = none, 8/9 = home-return fault
    void setPositioningStall(bool stall); // motor never reaches position
    // Physical estop stuck: M0 stays 1 even when the HMI clears M100 (simulates
    // a physical estop holding M0 despite the M100=0 release write).
    void setEstopReleaseStuck(bool stuck);

    // Safe abort of an in-flight width-adjust run (M34/M44 cleared, M45 set,
    // no late completion/timeout). Used by the software-estop path and by the
    // gateway when the link converges offline (PLC-HMI-005 D5).
    void abortWidthAdjust();

    // Advance simulated time by `seconds`, driving the D140 heartbeat,
    // positioning progress, the fixed 30 s width timeout and home return.
    void advance(quint64 seconds);

private:
    void onM43RisingEdge();
    void onM103RisingEdge();
    void onM104Write(bool value);
    void onM101RisingEdge();
    void onM102RisingEdge();
    void onM100Write(bool value);
    void updateM60();
    void updateD210();
    void updateD126();
    // Clamp D220 to the decoded PLC-visible maximum (15) and refresh
    // D126/D127 from it.
    void clampD220();
    // PLC status words synthesized from the M-coils (single source of truth
    // shared by the in-process gateway and the standalone RTU server). HMI
    // access to D100/D103 is read-only (address table, spec §8.2).
    // D100 bit0-7=M0-M7, bit8=M8|M60, bit9=M9|M61, bit10-14=M10-M14, bit15=0;
    // D103 bit0-15=M30-M45.
    quint16 statusWord1() const; // D100
    quint16 statusWord3() const; // D103
    void tick(quint64 seconds);

    SimulationClock &m_clock;

    // Coil storage (index = protocol address, extent 0-112). Address 112 is
    // in range but unused after the M112 removal; includes derived state bits
    // (M0, M1, M2, M60, M61) kept in sync by the handlers.
    bool m_coils[118] = {}; // 0..117 (M112 unused, M114-M117 test signals)

    // Holding registers D100-D223 (index = protocol address). D100/D103 are
    // read-only derived status words computed on read from the coils; writes
    // to them are ignored (see statusWord1/statusWord3).
    quint16 m_regs[224] = {};

    // Positioning progress: remaining seconds of the current run and the
    // fixed T6 K300 (30 s) timer accumulator in 100 ms units.
    quint64 m_remaining = 0;
    quint64 m_t6Elapsed = 0;
    bool m_positioning = false;
    bool m_stall = false; // injected stall: motor never reaches position
    // T6 done bit (user decision 2026-09-22). The decoded SBR_MANUALWIDTH
    // drives `T6 K300` and resets it only when D128 == D130, so a width
    // timeout stays latched until the width actually reaches the target; the
    // decoded M60 rung in SBR_FAULT uses ¬T6 as one of its terms.
    bool m_t6Done = false;

    // Home return: remaining seconds until completion.
    quint64 m_homeRemaining = 0;
    int m_homeFault = 0; // injected fault code (0 = none)
    bool m_estopReleaseStuck = false; // physical estop holds M0 despite M100=0
};

} // namespace hlm
