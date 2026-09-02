#pragma once

#include <QtGlobal>

#include "adapters/simulator/simulation_clock.h"

namespace hlm {

// Shared state model of the H3U PLC (spec §14.1). Simulates the control
// behavior of spec §10.2-§10.6 and the corrected reference ladder of
// §10.3.1: home return, width adjustment (M43/M34/M44/M45), dynamic
// timeout, mode switching, start/stop, software estop, D140 heartbeat and
// the M112 watchdog. Time is injected via SimulationClock and advanced
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

    // Advance simulated time by `seconds`, driving the D140 heartbeat,
    // positioning progress, dynamic timeout, home return and M112 watchdog.
    void advance(quint64 seconds);

private:
    void onM43RisingEdge();
    void onM103RisingEdge();
    void onM104Write(bool value);
    void onM101RisingEdge();
    void onM102RisingEdge();
    void onM100Write(bool value);
    void onM112Write(bool value);
    void updateM49();
    void updateM60();
    void updateD210();
    void updateD126();
    void tick(quint64 seconds);

    SimulationClock &m_clock;

    // Coils M0-M112 (index = protocol address). Includes derived state bits
    // (M0, M1, M2, M60, M61) kept in sync by the handlers.
    bool m_coils[113] = {};

    // Holding registers D100-D223 (index = protocol address).
    quint16 m_regs[224] = {};

    // Continuous safety interlock during width adjustment (spec §10.3.1 M49).
    bool m_m49 = false;

    // Positioning progress: remaining seconds of the current run and the
    // T6 100 ms timer accumulator (preset D222).
    quint64 m_remaining = 0;
    quint64 m_t6Elapsed = 0;
    bool m_positioning = false;
    bool m_stall = false; // injected stall: motor never reaches position

    // Home return: remaining seconds until completion.
    quint64 m_homeRemaining = 0;
    int m_homeFault = 0; // injected fault code (0 = none)
    bool m_estopReleaseStuck = false; // physical estop holds M0 despite M100=0

    // M112 watchdog: seconds since the last M112 rising edge.
    quint64 m_watchdogElapsed = 0;
    bool m_watchdogArmed = false;
};

} // namespace hlm
