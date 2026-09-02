// Task 3 unit tests: shared H3U simulation state model (spec §10.2-§10.6,
// §10.3.1, §14.1). Deterministic: time is injected via SimulationClock and
// advanced explicitly; no real sleeps.
//
// Coverage required by the task brief:
// - M43/M44/M45 mutual exclusion (success only M44, failure only M45).
// - Target latch: D128 changed during positioning does not affect the run,
//   D130 ends at the latched value.
// - SUB/MUL must not overwrite the D138/D139 production count.
// - Dynamic timeout (10s / 360s clamp boundaries).
// - Home-return faults 8/9.
// - Width-adjust timeout fault 10 (M45, M14, D110=10).
// - M112 watchdog: 2 s without an edge clears M42/M106-M111 while
//   M100/M104/M105 stay.
// - Mode mutual exclusion: mode switch rejected while running.
// - Software estop sets M0 and latches the fault.
// - Start (M60 satisfied -> M3=1; not satisfied -> invalid) and stop M102.

#include <QtTest>

#include "adapters/simulator/h3u_simulation_model.h"
#include "adapters/simulator/simulation_clock.h"

using namespace hlm;

namespace {

// Reset + home return to a ready state (M61=1, M60=1). Home return takes
// 2 simulated seconds.
void homeReady(H3uSimulationModel &m)
{
    m.writeCoil(103, true);
    m.writeCoil(103, false);
    m.advance(2);
}

} // namespace

class H3uSimulationModelTest : public QObject
{
    Q_OBJECT

private slots:
    void m43SuccessOnlyM44();
    void m43PreconditionFailureOnlyM45();
    void m43WhileBusyAbortsOnlyM45();
    void m43NeverBothM44AndM45();
    void targetLatchDuringPositioning();
    void subMulDoesNotOverwriteProduction();
    void dynamicTimeoutLowerBound();
    void dynamicTimeoutUpperBound();
    void homeReturnFault8();
    void homeReturnFault9();
    void widthAdjustTimeoutFault10();
    void m112WatchdogClearsBits();
    void modeSwitchRejectedWhileRunning();
    void softwareEstopLatchesFault();
    void startRequiresReady();
    void stopClearsRunning();
    void d140IncrementsAndWraps();
    void m103ClearsWidthResults();
    void d126IsSpeedTimesPulsePerMm();
    void d210SignedDelta();
};

void H3uSimulationModelTest::m43SuccessOnlyM44()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15); // 15 mm/s
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);

    QVERIFY(m.readCoil(34)); // adjusting
    QVERIFY(!m.readCoil(44));
    QVERIFY(!m.readCoil(45));
    QCOMPARE(m.readRegister(212), quint16(300)); // latched target

    m.advance(7); // ceil(100 / 15) = 7 s
    QVERIFY(!m.readCoil(34));
    QVERIFY(m.readCoil(44));
    QVERIFY(!m.readCoil(45));
    QCOMPARE(m.readRegister(130), quint16(300));
}

void H3uSimulationModelTest::m43PreconditionFailureOnlyM45()
{
    // Not homed: interlock fails -> only M45.
    SimulationClock clock;
    H3uSimulationModel m(clock);
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QVERIFY(m.readCoil(45));
    QVERIFY(!m.readCoil(44));
    QVERIFY(!m.readCoil(34));

    // D128 == D130: invalid command -> only M45.
    SimulationClock clock2;
    H3uSimulationModel m2(clock2);
    homeReady(m2);
    m2.writeCoil(43, true);
    m2.writeCoil(43, false);
    QVERIFY(m2.readCoil(45));
    QVERIFY(!m2.readCoil(44));
    QVERIFY(!m2.readCoil(34));
}

void H3uSimulationModelTest::m43WhileBusyAbortsOnlyM45()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15);
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QVERIFY(m.readCoil(34));

    // Second M43 while busy: abort current positioning, only M45.
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QVERIFY(!m.readCoil(34));
    QVERIFY(m.readCoil(45));
    QVERIFY(!m.readCoil(44));

    // Aborted run must not complete later.
    m.advance(10);
    QVERIFY(!m.readCoil(34));
    QVERIFY(m.readCoil(45));
    QVERIFY(!m.readCoil(44));
    QCOMPARE(m.readRegister(130), quint16(200)); // unchanged
}

void H3uSimulationModelTest::m43NeverBothM44AndM45()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15);

    // Success path.
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    m.advance(7);
    QVERIFY(!(m.readCoil(44) && m.readCoil(45)));

    // Failure path (D128 == D130 now).
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QVERIFY(!(m.readCoil(44) && m.readCoil(45)));
    QVERIFY(m.readCoil(45));

    // Busy-abort path.
    m.writeRegister(128, 250);
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QVERIFY(m.readCoil(34));
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QVERIFY(!(m.readCoil(44) && m.readCoil(45)));
    QVERIFY(m.readCoil(45));
}

void H3uSimulationModelTest::targetLatchDuringPositioning()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15);
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QCOMPARE(m.readRegister(212), quint16(300));

    // External D128 change during positioning must not affect this run.
    m.writeRegister(128, 350);
    m.advance(7);

    QCOMPARE(m.readRegister(130), quint16(300)); // latched target, not 350
    QCOMPARE(qint16(m.readRegister(210)), qint16(50)); // 350 - 300
    QVERIFY(m.readCoil(44));
    QVERIFY(!m.readCoil(45));
}

void H3uSimulationModelTest::subMulDoesNotOverwriteProduction()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15);
    m.setProductionCount(12345);
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);

    // D136/D137 = diff * D204 = 100 * 1280 = 128000 (signed 32-bit).
    QCOMPARE(m.readRegister32(136), quint32(128000));
    m.advance(7);
    QCOMPARE(m.readRegister32(136), quint32(128000));
    // D138/D139 production count untouched by SUB/MUL.
    QCOMPARE(m.readRegister32(138), quint32(12345));
}

void H3uSimulationModelTest::dynamicTimeoutLowerBound()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15);
    m.writeRegister(128, 201); // diff = 1
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    // ceil(1/15) = 1 -> 1 + 5 = 6 -> clamp to 10.
    QCOMPARE(m.readRegister(218), quint16(10));
    QCOMPARE(m.readRegister(222), quint16(100)); // 10 * 10 (100 ms units)
}

void H3uSimulationModelTest::dynamicTimeoutUpperBound()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(130, 0); // extreme start width for the clamp test
    m.writeRegister(220, 1);
    m.writeRegister(128, 400); // diff = 400
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    // ceil(400/1) = 400 -> 405 -> clamp to 360.
    QCOMPARE(m.readRegister(218), quint16(360));
    QCOMPARE(m.readRegister(222), quint16(3600));
}

void H3uSimulationModelTest::homeReturnFault8()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    m.setHomeReturnFault(8);
    m.writeCoil(103, true);
    m.writeCoil(103, false);
    QVERIFY(m.readCoil(50)); // homing
    m.advance(2);
    QVERIFY(!m.readCoil(50));
    QVERIFY(!m.readCoil(61));
    QVERIFY(m.readCoil(14));
    QCOMPARE(m.readRegister(110), quint16(8));
}

void H3uSimulationModelTest::homeReturnFault9()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    m.setHomeReturnFault(9);
    m.writeCoil(103, true);
    m.writeCoil(103, false);
    m.advance(2);
    QVERIFY(!m.readCoil(61));
    QVERIFY(m.readCoil(14));
    QCOMPARE(m.readRegister(110), quint16(9));
}

void H3uSimulationModelTest::widthAdjustTimeoutFault10()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15);
    m.writeRegister(128, 300);
    m.setPositioningStall(true); // motor never reaches position
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    QCOMPARE(m.readRegister(218), quint16(12)); // ceil(100/15)+5 = 12
    m.advance(12);
    QVERIFY(!m.readCoil(34));
    QVERIFY(!m.readCoil(44));
    QVERIFY(m.readCoil(45));
    QVERIFY(m.readCoil(14));
    QCOMPARE(m.readRegister(110), quint16(10));
}

void H3uSimulationModelTest::m112WatchdogClearsBits()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    m.writeCoil(42, true);
    m.writeCoil(106, true);
    m.writeCoil(107, true);
    m.writeCoil(108, true);
    m.writeCoil(109, true);
    m.writeCoil(110, true);
    m.writeCoil(111, true);
    m.writeCoil(100, true); // estop: must survive
    m.writeCoil(104, true); // auto mode: must survive
    m.writeCoil(105, true); // passthrough: must survive
    m.writeCoil(112, true); // watchdog edge at t=0

    m.advance(1);
    QVERIFY(m.readCoil(42)); // 1 s since edge: still armed

    m.advance(1); // 2 s since edge: watchdog fires
    QVERIFY(!m.readCoil(42));
    QVERIFY(!m.readCoil(106));
    QVERIFY(!m.readCoil(107));
    QVERIFY(!m.readCoil(108));
    QVERIFY(!m.readCoil(109));
    QVERIFY(!m.readCoil(110));
    QVERIFY(!m.readCoil(111));
    QVERIFY(m.readCoil(100));
    QVERIFY(m.readCoil(104));
    QVERIFY(m.readCoil(105));
}

void H3uSimulationModelTest::modeSwitchRejectedWhileRunning()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeCoil(104, true); // auto
    QVERIFY(m.readCoil(2));
    QVERIFY(!m.readCoil(1));

    m.writeCoil(101, true);
    m.writeCoil(101, false);
    QVERIFY(m.readCoil(3));

    m.writeCoil(104, false); // rejected while running
    QVERIFY(m.readCoil(2));
    QVERIFY(!m.readCoil(1));

    m.writeCoil(102, true);
    m.writeCoil(102, false);
    QVERIFY(!m.readCoil(3));

    m.writeCoil(104, false); // now allowed
    QVERIFY(m.readCoil(1));
    QVERIFY(!m.readCoil(2));
}

void H3uSimulationModelTest::softwareEstopLatchesFault()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeCoil(104, true);
    m.writeCoil(101, true);
    m.writeCoil(101, false);
    QVERIFY(m.readCoil(3));

    m.writeCoil(100, true);
    QVERIFY(m.readCoil(0));
    QVERIFY(m.readCoil(14));
    QCOMPARE(m.readRegister(110), quint16(1));
    QVERIFY(!m.readCoil(3)); // running stopped

    // Release clears M0 only; the fault stays latched until reset.
    m.writeCoil(100, false);
    QVERIFY(!m.readCoil(0));
    QVERIFY(m.readCoil(14));
    QCOMPARE(m.readRegister(110), quint16(1));

    m.writeCoil(103, true);
    m.writeCoil(103, false);
    QVERIFY(!m.readCoil(14));
    QCOMPARE(m.readRegister(110), quint16(0));
}

void H3uSimulationModelTest::startRequiresReady()
{
    // Not ready (M60=0): start is invalid.
    SimulationClock clock;
    H3uSimulationModel m(clock);
    m.writeCoil(104, true);
    m.writeCoil(101, true);
    m.writeCoil(101, false);
    QVERIFY(!m.readCoil(3));

    // Ready: start sets M3.
    homeReady(m);
    m.writeCoil(101, true);
    m.writeCoil(101, false);
    QVERIFY(m.readCoil(3));
}

void H3uSimulationModelTest::stopClearsRunning()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeCoil(104, true);
    m.writeCoil(101, true);
    m.writeCoil(101, false);
    QVERIFY(m.readCoil(3));

    m.writeCoil(102, true);
    m.writeCoil(102, false);
    QVERIFY(!m.readCoil(3));
}

void H3uSimulationModelTest::d140IncrementsAndWraps()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    QCOMPARE(m.readRegister(140), quint16(0));
    m.advance(1);
    QCOMPARE(m.readRegister(140), quint16(1));
    m.advance(65535); // 1 + 65535 = 65536 -> 16-bit wrap to 0
    QCOMPARE(m.readRegister(140), quint16(0));
}

void H3uSimulationModelTest::m103ClearsWidthResults()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(220, 15);
    m.writeRegister(128, 300);
    m.writeCoil(43, true);
    m.writeCoil(43, false);
    m.advance(7);
    QVERIFY(m.readCoil(44));

    m.writeCoil(103, true);
    m.writeCoil(103, false);
    QVERIFY(!m.readCoil(44));
    QVERIFY(!m.readCoil(45));
    QVERIFY(!m.readCoil(34));
    QVERIFY(m.readCoil(50)); // home return restarted
}

void H3uSimulationModelTest::d126IsSpeedTimesPulsePerMm()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    m.writeRegister(220, 15);
    m.writeRegister(204, 1280);
    QCOMPARE(m.readRegister32(126), quint32(15 * 1280));
}

void H3uSimulationModelTest::d210SignedDelta()
{
    SimulationClock clock;
    H3uSimulationModel m(clock);
    homeReady(m);
    m.writeRegister(128, 150); // D130 = 200 -> delta = -50
    QCOMPARE(qint16(m.readRegister(210)), qint16(-50));
}

QTEST_GUILESS_MAIN(H3uSimulationModelTest)
#include "test_h3u_simulation_model.moc"
