// PLC-HMI-003 black-box tests: evidence-based poll-block quality and age,
// overall age/quality aggregation, staleness of partial data and independent
// D210/WidthDelta validity (brief OB-8, OB-9, OB-10).
//
// Quality/age/validity are read and written through the contract-fixed
// snapshot member names (fast_quality, fast_age_ms, home_quality, home_age_ms,
// command_quality, command_age_ms, slow_quality, slow_age_ms, overall_quality,
// overall_age_ms, width_delta_valid); no naming fallback exists, so a compile
// failure on the current tree is the expected RED.
//
// Authored from the behavior-only brief .ai/test-briefs/PLC-HMI-003.yaml and
// the approved .ai/project-contract.yaml. No production implementation source
// was read.

#include <QtTest>

#include <QVector>

#include <algorithm>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "domain/device_snapshot.h"

using namespace hlm;

namespace {

constexpr quint16 kD128 = 128;
constexpr quint16 kD130 = 130;
constexpr quint16 kD210 = 210;

void advanceUntilOnline(SimulatedPlcGateway &gw, int maxTicks = 10)
{
    for (int i = 0; i < maxTicks && !gw.isOnline(); ++i)
        gw.tick();
}

DeviceSnapshotData validData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;   // D128
    d.currentWidth = 150;  // D130
    d.widthDelta = 50;     // D210
    d.pulsePerMm = 1280;   // D204
    d.widthSpeed = 15;     // D220
    d.beltSpeed = 1000;    // D122
    d.heartbeat = 1;       // D140
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

} // namespace

class PlcSnapshotQualityTest : public QObject
{
    Q_OBJECT

private slots:
    // --- OB-8: real transfer outcomes drive block quality and age -------------
    void successfulPollMarksEveryRequiredBlockValidAndOverallAgeIsTheMaximum();
    void failedTransfersNeverPresentValidDataAndSuccessfulRefreshRecovers();
    void overallAgeIsMaximumOfBlockAgesAndNeverHardCodedZero();

    // --- OB-10: stale or partial data is not reported fresh -------------------
    void partiallyStaleOrFailedDataIsNotOverallValid();

    // --- OB-9: D210 has independent validity metadata --------------------------
    void widthDeltaValidityRequiresSlowBlockAndSignedRange();
    void widthDeltaValidityIsIndependentOfCurrentWidthValidity();
};

// --- OB-8 ---------------------------------------------------------------------

void PlcSnapshotQualityTest::successfulPollMarksEveryRequiredBlockValidAndOverallAgeIsTheMaximum()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());
    for (int i = 0; i < 3; ++i)
        gw.tick();

    const DeviceSnapshot snap = gw.lastSnapshot();

    QVERIFY2(snap.fast_quality == DataQuality::Valid,
             "a successful fast poll must mark the fast block valid");
    QVERIFY2(snap.home_quality == DataQuality::Valid,
             "a successful home poll must mark the home block valid");
    QVERIFY2(snap.command_quality == DataQuality::Valid,
             "a successful command poll must mark the command block valid");
    QVERIFY2(snap.slow_quality == DataQuality::Valid,
             "a successful slow poll must mark the slow block valid");

    const qint64 fastAge = snap.fast_age_ms;
    const qint64 homeAge = snap.home_age_ms;
    const qint64 commandAge = snap.command_age_ms;
    const qint64 slowAge = snap.slow_age_ms;
    QVERIFY2(fastAge >= 0, "fast block age must be a real elapsed time");
    QVERIFY2(homeAge >= 0, "home block age must be a real elapsed time");
    QVERIFY2(commandAge >= 0, "command block age must be a real elapsed time");
    QVERIFY2(slowAge >= 0, "slow block age must be a real elapsed time");

    const qint64 maximumAge = std::max({fastAge, homeAge, commandAge, slowAge});
    QCOMPARE(snap.overall_age_ms, maximumAge);
}

void PlcSnapshotQualityTest::failedTransfersNeverPresentValidDataAndSuccessfulRefreshRecovers()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());
    gw.tick();
    QVERIFY2(gw.lastSnapshot().fast_quality == DataQuality::Valid,
             "precondition: the fast block starts valid");

    QVector<DataQuality> qualitiesDuringFailure;
    connect(&gw, &SimulatedPlcGateway::snapshotReady, this,
            [&qualitiesDuringFailure](quint64, const DeviceSnapshot &s) {
                qualitiesDuringFailure.append(s.fast_quality);
            });

    gw.setLinkDown(true);
    for (int i = 0; i < 5; ++i)
        gw.tick();
    QVERIFY2(!gw.isOnline(), "the simulated link did not reach the offline state");

    for (DataQuality quality : qualitiesDuringFailure) {
        QVERIFY2(quality != DataQuality::Valid,
                 "a failed transfer was presented as a valid block");
    }

    // A successful refresh clears the failure on the affected block.
    gw.setLinkDown(false);
    for (int i = 0; i < 10 && !gw.isOnline(); ++i)
        gw.tick();
    QVERIFY(gw.isOnline());
    gw.tick();
    gw.tick();
    QVERIFY2(gw.lastSnapshot().fast_quality == DataQuality::Valid,
             "a successful refresh must clear the failed block");
}

void PlcSnapshotQualityTest::overallAgeIsMaximumOfBlockAgesAndNeverHardCodedZero()
{
    DeviceSnapshotData d = validData();
    d.fast_age_ms = 111;
    d.home_age_ms = 222;
    d.command_age_ms = 333;
    d.slow_age_ms = 444;

    const DeviceSnapshot snap(d);
    QCOMPARE(snap.overall_age_ms, qint64(444));

    // A different maximum must be reflected: the value may not be a constant.
    d.fast_age_ms = 999;
    QCOMPARE(DeviceSnapshot(d).overall_age_ms, qint64(999));
    QVERIFY(DeviceSnapshot(d).overall_age_ms != 0);
}

// --- OB-10 --------------------------------------------------------------------

void PlcSnapshotQualityTest::partiallyStaleOrFailedDataIsNotOverallValid()
{
    DeviceSnapshotData d = validData();
    QVERIFY2(aggregateQuality(d) == DataQuality::Valid,
             "a fully valid snapshot must aggregate to Valid");

    d.slow_quality = DataQuality::Stale;
    QVERIFY2(aggregateQuality(d) != DataQuality::Valid,
             "a stale slow block must not aggregate to Valid");

    d.slow_quality = DataQuality::ProtocolError;
    QVERIFY2(aggregateQuality(d) != DataQuality::Valid,
             "a failed slow block must not aggregate to Valid");

    d.slow_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Stale;
    QVERIFY2(aggregateQuality(d) != DataQuality::Valid,
             "a stale command block must not aggregate to Valid");

    d.command_quality = DataQuality::Valid;
    QVERIFY2(aggregateQuality(d) == DataQuality::Valid,
             "recovering every block must aggregate to Valid again");
}

// --- OB-9 ---------------------------------------------------------------------

void PlcSnapshotQualityTest::widthDeltaValidityRequiresSlowBlockAndSignedRange()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());
    gw.tick();

    struct DeltaCase
    {
        quint16 target;  // D128
        quint16 current; // D130
        qint16 delta;    // D210
        bool expectedValid;
        const char *why;
    };

    const QVector<DeltaCase> cases{
        {200, 150, 50, true, "in-range positive D210 must be valid"},
        {400, 50, 350, true, "upper boundary +350 must be valid"},
        {50, 400, -350, true, "lower boundary -350 must be valid"},
        {200, 0, 200, true, "in-range D210 stays valid with an out-of-range D130"},
        {401, 50, 351, false, "D210 above +350 must be invalid"},
        {50, 401, -351, false, "D210 below -350 must be invalid"},
    };

    for (const DeltaCase &c : cases) {
        // Write the widths and the delta consistently so the assertion holds
        // whether the simulator stores D210 verbatim or derives it from
        // D128 - D130.
        gw.model().writeRegister(kD128, c.target);
        gw.model().writeRegister(kD130, c.current);
        gw.model().writeRegister(kD210, static_cast<quint16>(c.delta));
        gw.tick();
        QVERIFY2(gw.lastSnapshot().width_delta_valid == c.expectedValid, c.why);
    }

    // A failed slow transfer invalidates the delta metadata as well.
    QVector<bool> deltasDuringFailure;
    connect(&gw, &SimulatedPlcGateway::snapshotReady, this,
            [&deltasDuringFailure](quint64, const DeviceSnapshot &s) {
                deltasDuringFailure.append(s.width_delta_valid);
            });
    gw.setLinkDown(true);
    for (int i = 0; i < 5; ++i)
        gw.tick();
    for (bool valid : deltasDuringFailure) {
        QVERIFY2(!valid,
                 "width-delta validity must require a valid slow block");
    }
}

void PlcSnapshotQualityTest::widthDeltaValidityIsIndependentOfCurrentWidthValidity()
{
    SimulatedPlcGateway gw;
    gw.start();
    advanceUntilOnline(gw);
    QVERIFY(gw.isOnline());
    gw.tick();

    // D130 = 0 is outside the authoritative 50..400 width range, so
    // CurrentWidth is invalid; D210 = 200 is inside the signed -350..350
    // range with a valid slow block, so the delta metadata must stay valid.
    gw.model().writeRegister(kD128, 200);
    gw.model().writeRegister(kD130, 0);
    gw.model().writeRegister(kD210, 200);
    gw.tick();

    QVERIFY2(gw.lastSnapshot().width_delta_valid,
             "D210 validity must not alias CurrentWidth (D130) validity");
}

QTEST_GUILESS_MAIN(PlcSnapshotQualityTest)
#include "plc_snapshot_quality_test.moc"
