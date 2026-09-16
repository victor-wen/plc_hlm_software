// PLC-HMI-005 black-box unit tests: PLC-authoritative simulator parity --
// authoritative startup defaults, the D126/D127 frequency formula, the fixed
// 30 s (T6 K300) width-adjust timeout with no dynamic or M49-dependent timing,
// and no M112 parity semantics (brief OB-1, OB-2, OB-3, OB-6).
//
// Authored only from .ai/test-briefs/PLC-HMI-005.yaml, the approved
// .ai/project-contract.yaml and inspectable test sources under tests/**. No
// production implementation source was read.
//
// Expected RED: runtime assertion failures against the current simulator,
// which exposes the obsolete defaults (D122=1000, D204=1280, D220=20), the
// obsolete D126/D127 = D220*D204 formula and a dynamic width-adjust timeout.

#include <QtTest>

#include <algorithm>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "domain/device_snapshot.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM34 = 34;   // adjusting
constexpr quint16 kM43 = 43;   // width-adjust pulse
constexpr quint16 kM44 = 44;   // adjust success
constexpr quint16 kM45 = 45;   // adjust failure / timeout
constexpr quint16 kM49 = 49;   // unoccupied bit: must be parity-neutral
constexpr quint16 kM103 = 103; // reset / home pulse
constexpr quint16 kM112 = 112; // removed watchdog bit: must never be energized

constexpr quint16 kD122 = 122; // belt speed
constexpr quint16 kD126 = 126; // frequency word pair
constexpr quint16 kD127 = 127;
constexpr quint16 kD128 = 128; // target width
constexpr quint16 kD130 = 130; // current width
constexpr quint16 kD204 = 204; // pulse per mm
constexpr quint16 kD220 = 220; // width speed

// PLC-authoritative constants (brief OB-1/OB-2; contract H3UPlcContract).
constexpr quint32 kFixedK1280 = 1280;
constexpr quint16 kD122Default = 5000;
constexpr quint16 kD204Default = 128;
constexpr quint16 kD220EffectiveDefault = 15;
// Simulator default target width of a fresh machine, documented by the
// inspectable PLC-HMI-008 test source (restricted_mode_routing_test.cpp).
constexpr quint16 kD128Default = 200;

void advanceUntilOnline(SimulatedPlcGateway &gw, int maxTicks = 10)
{
    for (int i = 0; i < maxTicks && !gw.isOnline(); ++i)
        gw.tick();
}

void homeReady(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

// D126/D127 are a 16-bit word pair holding one frequency value. The brief
// fixes the value, not the word order, so both compositions are accepted for
// values that fit in one word.
struct FrequencyRead
{
    quint32 wordAt126First = 0;
    quint32 wordAt127First = 0;
};

FrequencyRead readFrequency(const SimulatedPlcGateway &gw)
{
    const quint32 at126 = gw.model().readRegister(kD126);
    const quint32 at127 = gw.model().readRegister(kD127);
    FrequencyRead read;
    read.wordAt126First = at126 | (at127 << 16);
    read.wordAt127First = at127 | (at126 << 16);
    return read;
}

bool frequencyMatches(const SimulatedPlcGateway &gw, quint32 expected)
{
    const FrequencyRead read = readFrequency(gw);
    return read.wordAt126First == expected || read.wordAt127First == expected;
}

bool frequencyWordEquals(const SimulatedPlcGateway &gw, quint32 candidate)
{
    const FrequencyRead read = readFrequency(gw);
    return read.wordAt126First == candidate || read.wordAt127First == candidate;
}

struct AdjustOutcome
{
    bool started = false;
    bool failedAtStart = false;
    bool earlyFailure = false;
    int failTick = -1; // tick index (1-based, after the start tick) of M45
    bool success = false;
    bool stillAdjusting = false;
};

// Runs one width adjust that cannot finish within the fixed timeout window:
// the target is the maximum documented delta (350 mm) and the pulse
// configuration is chosen so that the ideal motion time exceeds 30 s.
// No fault-injection hook is used; this is the parity path.
AdjustOutcome runWidthAdjustWithoutCompletion(quint16 d204, quint16 d220, bool m49Set)
{
    AdjustOutcome out;
    SimulatedPlcGateway gw;
    gw.start();
    for (int i = 0; i < 10 && !gw.isOnline(); ++i)
        gw.tick();
    if (!gw.isOnline())
        return out;

    homeReady(gw);

    gw.model().writeRegister(kD204, d204);
    gw.model().writeRegister(kD220, d220);
    if (m49Set)
        gw.model().writeCoil(kM49, true);

    const quint16 current = gw.model().readRegister(kD130);
    const quint16 target = quint16(std::min<int>(int(current) + 350, 32767));
    gw.model().writeRegister(kD128, target);
    gw.model().writeCoil(kM43, true);
    gw.model().writeCoil(kM43, false);
    gw.tick();

    out.started = gw.model().readCoil(kM34);
    out.failedAtStart = gw.model().readCoil(kM45);
    if (!out.started)
        return out;

    // The fixed T6 K300 timeout is 30 s (30 one-second steps). It must not
    // report failure before 29 steps...
    for (int tick = 1; tick <= 28; ++tick) {
        gw.tick();
        if (gw.model().readCoil(kM45)) {
            out.earlyFailure = true;
            out.failTick = tick;
            return out;
        }
    }
    // ... and must report the timeout within a small tick-alignment tolerance
    // after 30 steps.
    for (int tick = 29; tick <= 40; ++tick) {
        gw.tick();
        if (gw.model().readCoil(kM45)) {
            out.failTick = tick;
            break;
        }
    }

    out.success = gw.model().readCoil(kM44);
    out.stillAdjusting = gw.model().readCoil(kM34);
    return out;
}

struct AdjustCompletion
{
    bool started = false;
    bool failed = false;
    int failTick = -1;      // tick index (1-based) of M45, when it fired
    int completedTick = -1; // tick index (1-based) when M34 cleared
    bool success = false;   // M44 at the end of the observation window
};

// Runs one width adjust whose ideal motion time stays inside the fixed 30 s
// window: motion speed is D220 * 1280 pulses/s and D204 pulses per mm, so a
// 280 mm delta at D204=128/D220=1 takes 28 s. Such a run must complete
// successfully and must never report the T6 K300 timeout.
AdjustCompletion runWidthAdjustToCompletion(quint16 d204, quint16 d220,
                                            quint16 deltaMm)
{
    AdjustCompletion out;
    SimulatedPlcGateway gw;
    gw.start();
    for (int i = 0; i < 10 && !gw.isOnline(); ++i)
        gw.tick();
    if (!gw.isOnline())
        return out;

    homeReady(gw);
    gw.model().writeRegister(kD204, d204);
    gw.model().writeRegister(kD220, d220);

    const int current = int(gw.model().readRegister(kD130));
    const quint16 target = quint16(std::min<int>(current + int(deltaMm), 32767));
    gw.model().writeRegister(kD128, target);
    gw.model().writeCoil(kM43, true);
    gw.model().writeCoil(kM43, false);
    gw.tick();

    out.started = gw.model().readCoil(kM34);
    if (!out.started)
        return out;

    for (int tick = 1; tick <= 40; ++tick) {
        gw.tick();
        if (gw.model().readCoil(kM45)) {
            out.failed = true;
            out.failTick = tick;
            break;
        }
        if (!gw.model().readCoil(kM34)) {
            out.completedTick = tick;
            break;
        }
    }
    out.success = gw.model().readCoil(kM44);
    return out;
}

} // namespace

class PlcHmi005SimulatorParityTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-1: PLC-authoritative startup defaults -----------------------------
    void startupRegistersExposePlcAuthoritativeDefaults();

    // --- OB-2: D126/D127 = D220 * fixed K1280, never D220 * D204 --------------
    void frequencyRegistersEqualD220TimesFixedK1280();
    void frequencyRegistersNeverFollowTheD204Product();

    // --- verify-phase edge cases ----------------------------------------------
    void frequencyRegistersUpdateForD220BoundaryWrites();
    void frequencyRegistersHoldAcrossExtremeButValidD204Writes();

    // --- OB-3: fixed 30 s (T6 K300) width-adjust timeout -----------------------
    void widthAdjustTimeoutIsFixedAtThirtySecondsRegardlessOfM49();
    void widthAdjustTimeoutDoesNotDependOnThePulseConfiguration();

    // --- verify-phase edge case -----------------------------------------------
    void widthAdjustCompletingInsideTheWindowReportsSuccessWithoutTimeout();

    // --- OB-6: no M112 parity semantics ----------------------------------------
    void parityPathNeverEnergizesM112();
};

// --- OB-1 ---------------------------------------------------------------------

void PlcHmi005SimulatorParityTest::startupRegistersExposePlcAuthoritativeDefaults()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());

    QCOMPARE(int(gw.model().readRegister(kD122)), int(kD122Default));
    QCOMPARE(int(gw.model().readRegister(kD204)), int(kD204Default));
    QCOMPARE(int(gw.model().readRegister(kD220)), int(kD220EffectiveDefault));
    QCOMPARE(int(gw.model().readRegister(kD128)), int(kD128Default));

    // D220 starts at 20 and is clamped to the effective 15; the effective
    // value must stay 15 over the session.
    for (int i = 0; i < 5; ++i) {
        gw.tick();
        QCOMPARE(int(gw.model().readRegister(kD220)), int(kD220EffectiveDefault));
    }
    QCOMPARE(int(gw.model().readRegister(kD122)), int(kD122Default));
    QCOMPARE(int(gw.model().readRegister(kD204)), int(kD204Default));
    QCOMPARE(int(gw.model().readRegister(kD128)), int(kD128Default));

    // The decoded snapshot must carry the same authoritative values.
    const DeviceSnapshot snap = gw.lastSnapshot();
    QCOMPARE(int(snap.beltSpeed()), int(kD122Default));
    QCOMPARE(int(snap.pulsePerMm()), int(kD204Default));
    QCOMPARE(int(snap.widthSpeed()), int(kD220EffectiveDefault));
    QCOMPARE(int(snap.targetWidth()), int(gw.model().readRegister(kD128)));
    QCOMPARE(int(snap.currentWidth()), int(gw.model().readRegister(kD130)));
}

// --- OB-2 ---------------------------------------------------------------------

void PlcHmi005SimulatorParityTest::frequencyRegistersEqualD220TimesFixedK1280()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    gw.tick();

    // Fresh startup: D220 effective 15 -> 15 * 1280 = 19200.
    QVERIFY2(frequencyMatches(gw, kD220EffectiveDefault * kFixedK1280),
             qPrintable(QStringLiteral("startup frequency was %1 / %2, expected %3")
                            .arg(readFrequency(gw).wordAt126First)
                            .arg(readFrequency(gw).wordAt127First)
                            .arg(kD220EffectiveDefault * kFixedK1280)));

    // A valid D220 write within 1..15 recomputes the frequency.
    for (const quint16 speed : {quint16(10), quint16(1), quint16(15)}) {
        gw.model().writeRegister(kD220, speed);
        gw.tick();
        QVERIFY2(frequencyMatches(gw, quint32(speed) * kFixedK1280),
                 qPrintable(QStringLiteral("D220=%1 produced frequency %2 / %3, "
                                           "expected %4")
                                .arg(speed)
                                .arg(readFrequency(gw).wordAt126First)
                                .arg(readFrequency(gw).wordAt127First)
                                .arg(quint32(speed) * kFixedK1280)));
    }
}

void PlcHmi005SimulatorParityTest::frequencyRegistersNeverFollowTheD204Product()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    gw.tick();

    // D204 changes must not affect the frequency at all.
    gw.model().writeRegister(kD204, 300);
    gw.tick();
    QVERIFY2(frequencyMatches(gw, kD220EffectiveDefault * kFixedK1280),
             "a D204 write changed the D126/D127 frequency");
    QVERIFY2(!frequencyWordEquals(gw, quint32(kD220EffectiveDefault) * 300),
             "the frequency was derived from D220 * D204");

    // D220=10 with D204=300: authoritative value 12800, product 3000.
    gw.model().writeRegister(kD220, 10);
    gw.tick();
    QVERIFY2(frequencyMatches(gw, quint32(10) * kFixedK1280),
             "the frequency did not follow D220 * fixed K1280");
    QVERIFY2(!frequencyWordEquals(gw, quint32(10) * 300),
             "the frequency was derived from D220 * D204 instead of fixed K1280");
}

// Verify-phase edge cases derived from the brief's decoded ranges: D204 is
// validated/decoded over 1..32767 and D220 over 1..15, and D126/D127 must
// always equal the effective D220 multiplied by fixed K1280.

void PlcHmi005SimulatorParityTest::frequencyRegistersUpdateForD220BoundaryWrites()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    gw.tick();

    // D220 = 1 (minimum valid) -> 1280; D220 = 15 (maximum valid) -> 19200.
    for (const quint16 speed : {quint16(1), quint16(15)}) {
        gw.model().writeRegister(kD220, speed);
        gw.tick();
        QCOMPARE(int(gw.model().readRegister(kD220)), int(speed));
        QVERIFY2(frequencyMatches(gw, quint32(speed) * kFixedK1280),
                 qPrintable(QStringLiteral("D220=%1 produced frequency %2 / %3, "
                                           "expected %4")
                                .arg(speed)
                                .arg(readFrequency(gw).wordAt126First)
                                .arg(readFrequency(gw).wordAt127First)
                                .arg(quint32(speed) * kFixedK1280)));
        QVERIFY2(!frequencyWordEquals(gw, quint32(speed) * 128),
                 qPrintable(QStringLiteral("D220=%1 was multiplied by the D204 "
                                           "default instead of fixed K1280")
                                .arg(speed)));
    }
}

void PlcHmi005SimulatorParityTest::frequencyRegistersHoldAcrossExtremeButValidD204Writes()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    gw.tick();

    // D220 = 15 with D204 driven from its minimum to its maximum: the
    // frequency must stay 19200 and must never take a D204 product.
    gw.model().writeRegister(kD220, 15);
    gw.tick();
    for (const quint16 p : {quint16(1), quint16(128), quint16(32767)}) {
        gw.model().writeRegister(kD204, p);
        gw.tick();
        QVERIFY2(frequencyMatches(gw, quint32(15) * kFixedK1280),
                 qPrintable(QStringLiteral("D204=%1 changed the frequency away "
                                           "from 19200 (now %2 / %3)")
                                .arg(p)
                                .arg(readFrequency(gw).wordAt126First)
                                .arg(readFrequency(gw).wordAt127First)));
        QVERIFY2(!frequencyWordEquals(gw, quint32(15) * p),
                 qPrintable(QStringLiteral("the frequency followed D220 * D204 "
                                           "at D204=%1")
                                .arg(p)));
    }
}

// --- OB-3 ---------------------------------------------------------------------

void PlcHmi005SimulatorParityTest::widthAdjustTimeoutIsFixedAtThirtySecondsRegardlessOfM49()
{
    const AdjustOutcome baseline =
        runWidthAdjustWithoutCompletion(kD204Default, 1, false);
    QVERIFY2(baseline.started,
             "precondition failed: the adjust run did not start (M34 clear)");
    QVERIFY2(!baseline.failedAtStart,
             "the adjust reported failure on the start tick");
    QVERIFY2(!baseline.earlyFailure,
             "the width-adjust timeout reported failure before 29 s");
    QVERIFY2(baseline.failTick != -1,
             "no width-adjust timeout was reported within 40 s (fixed T6 K300 missing)");
    QVERIFY2(baseline.failTick <= 33,
             qPrintable(QStringLiteral("the timeout was reported at %1 s, after the "
                                       "fixed 30 s window")
                            .arg(baseline.failTick)));
    QVERIFY2(!baseline.success, "a timed-out adjust must not report success");
    QVERIFY2(!baseline.stillAdjusting,
             "the timed-out adjust must stop adjusting");

    // M49 is unoccupied in the parsed PLC project; it must not change the
    // fixed timeout.
    const AdjustOutcome withM49 =
        runWidthAdjustWithoutCompletion(kD204Default, 1, true);
    QVERIFY2(withM49.started, "the M49 run did not start");
    QVERIFY2(withM49.failTick != -1,
             "M49 suppressed the fixed 30 s timeout");
    QVERIFY2(qAbs(withM49.failTick - baseline.failTick) <= 1,
             qPrintable(QStringLiteral("M49 changed the timeout timing: %1 s vs %2 s")
                            .arg(withM49.failTick)
                            .arg(baseline.failTick)));
}

void PlcHmi005SimulatorParityTest::widthAdjustTimeoutDoesNotDependOnThePulseConfiguration()
{
    const AdjustOutcome baseline =
        runWidthAdjustWithoutCompletion(kD204Default, 1, false);
    QVERIFY2(baseline.failTick != -1,
             "no fixed timeout in the baseline pulse configuration");

    // A drastically longer ideal motion (D204 max, D220 max) must still time
    // out at the same fixed 30 s: an implementation that scales the timeout
    // with the pulse load is not parity.
    const AdjustOutcome extreme = runWidthAdjustWithoutCompletion(32767, 15, false);
    QVERIFY2(extreme.started, "the extreme pulse configuration did not start");
    QVERIFY2(extreme.failTick != -1,
             "the extreme pulse configuration produced no timeout (dynamic timeout)");
    QVERIFY2(qAbs(extreme.failTick - baseline.failTick) <= 1,
             qPrintable(QStringLiteral("the timeout depends on D204/D220: %1 s vs %2 s")
                            .arg(extreme.failTick)
                            .arg(baseline.failTick)));
    QVERIFY2(!extreme.success, "a timed-out adjust must not report success");
}

void PlcHmi005SimulatorParityTest::widthAdjustCompletingInsideTheWindowReportsSuccessWithoutTimeout()
{
    // D220=1 (fixed K1280) and D204=128 give 1280 pulses/s over 128 pulses/mm,
    // i.e. 10 mm/s; a 280 mm move takes ~28 s and must complete inside the
    // fixed 30 s window.
    const AdjustCompletion completion = runWidthAdjustToCompletion(128, 1, 280);
    QVERIFY2(completion.started,
             "precondition failed: the just-inside-window adjust did not start");
    QVERIFY2(!completion.failed,
             "an adjust that completes inside 30 s reported the T6 K300 timeout");
    QVERIFY2(completion.completedTick != -1,
             "the adjust did not complete within the 40 s observation window");
    QVERIFY2(completion.completedTick <= 31,
             qPrintable(QStringLiteral("the completion needed %1 s, outside the "
                                       "fixed 30 s window")
                            .arg(completion.completedTick)));
    QVERIFY2(completion.success,
             "a completed adjust inside the fixed window must report success");
}

// --- OB-6 ---------------------------------------------------------------------

void PlcHmi005SimulatorParityTest::parityPathNeverEnergizesM112()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());

    homeReady(gw);
    const quint16 current = gw.model().readRegister(kD130);
    gw.model().writeRegister(kD128, quint16(std::min<int>(int(current) + 100, 32767)));
    gw.model().writeCoil(kM43, true);
    gw.model().writeCoil(kM43, false);

    for (int i = 0; i < 40; ++i) {
        gw.tick();
        QVERIFY2(!gw.model().readCoil(kM112),
                 "coil 112 (M112) was energized on the parity path");
    }
    QVERIFY2(!gw.model().readCoil(kM112),
             "coil 112 (M112) was energized on the parity path");
}

QTEST_GUILESS_MAIN(PlcHmi005SimulatorParityTest)
#include "plc_hmi_005_simulator_parity_test.moc"
