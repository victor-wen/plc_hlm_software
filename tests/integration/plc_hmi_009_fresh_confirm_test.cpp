// PLC-HMI-009 OB-3 black-box integration tests (independent): a manual/hold/bypass
// command confirmation (M42, M105-M111) is only accepted from a snapshot whose
// source block carries fresh, valid evidence for that address. M42 is sourced
// from the fast block; M105-M111 from the command block. A stale or failed
// block that happens to show the requested value must not confirm the command;
// the pending confirmation instead converges through the existing defensive
// timeout with a non-success terminal and a non-empty detail.
//
// Authored only from .ai/test-briefs/PLC-HMI-009.yaml (brief_version 1), the
// approved .ai/project-contract.yaml (DeviceSnapshot quality rules; NF-03
// "No control command ... may be reported successful before confirmation from
// its authoritative peer"; OperatorCommandStatus invariants) and inspectable
// test sources under tests/**. No production implementation source was read.
//
// Observable surface used (every symbol appears in inspectable test sources
// under tests/**):
//   * SimulatedPlcGateway (start/tick/model()/lastSnapshot) as the snapshot and
//     connection source, with the ControlCoordinator PulseTransport seam of
//     tests/unit/operator_command_lifecycle_test.cpp;
//   * ControlCoordinator::onSnapshot, setRole, commandResult and the manual
//     command entry points used by the coordinator tests;
//   * DeviceSnapshotData/DeviceSnapshot construction with the contract-fixed
//     block quality/age members;
//   * the 3 s manual-confirmation defensive timeout established by
//     tests/unit/operator_lifecycle_developer_test.cpp (now += 3'001).
//
// Expected RED on the current tree: a snapshot whose command block (or fast
// block for M42) is stale/failed but shows the requested bit value still
// confirms the pending command, and a bit that already equals the request at
// dispatch time confirms immediately. Both are exactly what OB-3 forbids.

#include <QtTest>

#include <QString>
#include <QVector>

#include <functional>
#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "application/control_coordinator.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM42 = 42;   // fast-block continuous output (logout clear also clears it)
constexpr quint16 kM50 = 50;   // PLC-HMI-011: home-start coil
constexpr quint16 kM103 = 103; // reset / home pulse
constexpr quint16 kM106 = 106; // command-block manual hold output

// Established defensive confirmation timeout for manual commands (the
// developer test crosses it with now += 3'001).
constexpr qint64 kConfirmTimeoutMs = 3'000;

// --- coordinator test seam (verbatim convention of
// --- tests/unit/operator_command_lifecycle_test.cpp) --------------------------

quint64 nextRequestId()
{
    static quint64 next = 1;
    return next++;
}

SubmissionResult acceptedResult()
{
    SubmissionResult r;
    r.accepted = true;
    r.request_id = nextRequestId();
    r.gateway_generation = 1;
    return r;
}

SubmissionResult rejectedResult(const QString &reason)
{
    SubmissionResult r;
    r.accepted = false;
    r.request_id = 0;
    r.gateway_generation = 1;
    r.immediate_rejection_reason = reason;
    return r;
}

void homeReady(SimulatedPlcGateway &gw)
{
    gw.model().writeCoil(kM103, true);
    gw.model().writeCoil(kM103, false);
    gw.model().writeCoil(kM50, true); // PLC-HMI-011: homing starts on the home-start write
    gw.tick();
    gw.tick(); // home return takes 2 s
}

ControlCoordinator::PulseTransport gatewayTransport(SimulatedPlcGateway &gw)
{
    ControlCoordinator::PulseTransport t;
    t.startPulse = [&gw](quint16 a) -> SubmissionResult {
        gw.model().writeCoil(a, true);
        gw.model().writeCoil(a, false);
        return acceptedResult();
    };
    t.writeHold = [&gw](quint16 a, bool v) -> SubmissionResult {
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeCoil = [&gw](quint16 a, bool v, CommandPriority) -> SubmissionResult {
        gw.model().writeCoil(a, v);
        return acceptedResult();
    };
    t.writeRegister = [&gw](quint16 a, quint16 v, CommandPriority) -> SubmissionResult {
        gw.model().writeRegister(a, v);
        return acceptedResult();
    };
    return t;
}

ControlCoordinator *wire(SimulatedPlcGateway &gw, qint64 &now,
                         ControlCoordinator::PulseTransport t)
{
    auto *c = new ControlCoordinator(t, ControlCoordinator::Config(),
                                     [&now]() { return now; });
    QObject::connect(&gw, &SimulatedPlcGateway::snapshotReady, c,
                     [c](quint64, const DeviceSnapshot &s) { c->onSnapshot(s); });
    QObject::connect(&gw, &SimulatedPlcGateway::connectionStateChanged, c,
                     [c](quint64, bool online) { c->onConnectionChanged(online); });
    if (gw.hasSnapshot())
        c->onSnapshot(gw.lastSnapshot());
    return c;
}

ControlCoordinator *makeCoordinator(SimulatedPlcGateway &gw, qint64 &now)
{
    return wire(gw, now, gatewayTransport(gw));
}

// --- deterministic snapshot construction --------------------------------------
// DeviceSnapshotData packs the command-block coils M100..M111 into
// commandBits bits 0..11 (M106 is bit 6); the DeviceSnapshot constructor
// derives the per-bit accessors from the packed values. The quality/age
// members are set explicitly so no fabricated Valid evidence exists.

DeviceSnapshotData baseSnapshotData()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;  // D128
    d.currentWidth = 150; // D130
    d.widthDelta = 50;    // D210
    d.pulsePerMm = 128;   // D204
    d.widthSpeed = 15;    // D220
    d.beltSpeed = 5000;   // D122
    d.heartbeat = 1;      // D140
    d.fast_quality = DataQuality::Valid;
    d.fast_age_ms = 0;
    d.home_quality = DataQuality::Valid;
    d.home_age_ms = 0;
    d.command_quality = DataQuality::Valid;
    d.command_age_ms = 0;
    d.slow_quality = DataQuality::Valid;
    d.slow_age_ms = 0;
    return d;
}

// Command block (M105-M111 source): M106 at commandBits bit 6.
DeviceSnapshotData commandBlockSnapshot(bool m106, DataQuality commandQuality,
                                        qint64 commandAgeMs)
{
    DeviceSnapshotData d = baseSnapshotData();
    d.commandBits = m106 ? quint16(quint16(1) << 6) : quint16(0);
    d.command_quality = commandQuality;
    d.command_age_ms = commandAgeMs;
    d.overall_quality = aggregateQuality(d);
    return d;
}

// Fast block (M42 source). DeviceSnapshotData carries no per-bit M42 member;
// the fast-block status word packs the width/manual status coils. The mapping
// used here is derived from the readable test fixtures: M44 is bit 14 of
// statusWord3 (command_feedback_pages_test.cpp,
// command_status_projection_test.cpp: statusWord3 = 1 << 14 // M44), so the
// word covers M30..M45 and M42 is bit 12. This is a frozen author assumption
// recorded in the RED report; the paired Valid case below fails if the mapping
// is wrong instead of letting the stale case pass vacuously.
DeviceSnapshotData fastBlockSnapshot(bool m42, DataQuality fastQuality, qint64 fastAgeMs)
{
    DeviceSnapshotData d = baseSnapshotData();
    if (m42)
        d.statusWord3 |= quint16(quint16(1) << 12); // M42
    d.fast_quality = fastQuality;
    d.fast_age_ms = fastAgeMs;
    d.overall_quality = aggregateQuality(d);
    return d;
}

struct ResultRecord
{
    bool ok = true;
    QString detail;
};

int successCount(const QVector<ResultRecord> &results)
{
    int count = 0;
    for (const ResultRecord &r : results) {
        if (r.ok)
            ++count;
    }
    return count;
}

bool anyNonSuccessTerminalWithDetail(const QVector<ResultRecord> &results)
{
    for (const ResultRecord &r : results) {
        if (!r.ok && !r.detail.trimmed().isEmpty())
            return true;
    }
    return false;
}

QString describeResults(const QVector<ResultRecord> &results)
{
    QStringList parts;
    int index = 0;
    for (const ResultRecord &r : results) {
        parts.append(QStringLiteral("#%1 ok=%2 detail='%3'")
                         .arg(index++)
                         .arg(r.ok ? QStringLiteral("true") : QStringLiteral("false"))
                         .arg(r.detail));
    }
    return parts.join(QStringLiteral(" | "));
}

} // namespace

class PlcHmi009FreshConfirmTest : public QObject
{
    Q_OBJECT

private slots:
    // OB-3: a stale command block cannot confirm a just-issued hold.
    void staleCommandBlockCannotConfirmAJustIssuedHold();

    // OB-3: valid command-block evidence confirms exactly once.
    void freshCommandBlockConfirms();

    // OB-3: M42 is sourced from the fast block only.
    void staleFastBlockCannotConfirmM42Hold();

    // OB-3: M42 is sourced from the fast block only (already covered
    // negatively by staleFastBlockCannotConfirmM42Hold); the source block's
    // fresh evidence alone must be sufficient, with no cross-block acceptance
    // and no cross-block suppression.
    void sourceBlockEvidenceAloneConfirms();

    // OB-3: an unconfirmed command converges via the defensive timeout.
    void staleCommandBlockConvergesToNonSuccessTerminal();
};

// --- OB-3: stale command block must not confirm ---------------------------------

void PlcHmi009FreshConfirmTest::staleCommandBlockCannotConfirmAJustIssuedHold()
{
    // The requested bit already equals the machine state at dispatch time (the
    // stale-readback trap). The only evidence offered afterwards is a snapshot
    // whose command block is Stale, so OB-3 forbids any success within the
    // confirmation window.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    QVERIFY2(c != nullptr, "the coordinator must be constructible over the simulator");
    c->setRole(Role::Admin);
    homeReady(gw);

    // Pre-energized target: the readback already equals the requested value.
    gw.model().writeCoil(kM106, true);
    gw.tick();

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&results](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::ManualCommand)
                    results.append({ok, detail});
            });

    const ControlCoordinator::CommandResult issued = c->manualHold(kM106, true);
    QVERIFY2(issued.accepted,
             qPrintable(QStringLiteral("precondition: the manual command was not accepted (%1)")
                            .arg(issued.reason)));

    // Several advancing snapshots with stale command-block evidence, still
    // below the 3 s defensive confirmation timeout.
    now += 1'000;
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Stale, now)));
    now += 1'000;
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Stale, now)));
    now += 500;
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Stale, now)));
    QVERIFY2(now < kConfirmTimeoutMs, "the stale window must stay below the confirm timeout");

    QVERIFY2(successCount(results) == 0,
             qPrintable(QStringLiteral("a stale command block confirmed the just-issued manual "
                                       "hold: %1 success result(s) observed; sequence: %2")
                            .arg(successCount(results))
                            .arg(describeResults(results))));
}

// --- OB-3: fresh command-block evidence confirms exactly once -------------------

void PlcHmi009FreshConfirmTest::freshCommandBlockConfirms()
{
    // Same stale-readback trap, but the command block arrives Valid: the
    // command must confirm with exactly one success terminal carrying a
    // non-empty detail, and later snapshots must not duplicate it.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    QVERIFY2(c != nullptr, "the coordinator must be constructible over the simulator");
    c->setRole(Role::Admin);
    homeReady(gw);

    gw.model().writeCoil(kM106, true);
    gw.tick();

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&results](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::ManualCommand)
                    results.append({ok, detail});
            });

    const ControlCoordinator::CommandResult issued = c->manualHold(kM106, true);
    QVERIFY2(issued.accepted,
             qPrintable(QStringLiteral("precondition: the manual command was not accepted (%1)")
                            .arg(issued.reason)));

    now += 1'000;
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Valid, 0)));

    QVERIFY2(successCount(results) == 1,
             qPrintable(QStringLiteral("valid command-block evidence produced %1 success "
                                       "result(s) instead of exactly one; sequence: %2")
                            .arg(successCount(results))
                            .arg(describeResults(results))));
    QVERIFY2(!results.first().detail.trimmed().isEmpty(),
             "the confirmed manual command carries no visible detail");

    // Duplicate lock: further fresh snapshots must not produce a second
    // terminal for the same command.
    for (int i = 0; i < 3; ++i)
        c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Valid, 0)));
    QCOMPARE(results.size(), 1);
    QCOMPARE(successCount(results), 1);
}

// --- OB-3: M42 must be sourced from the fast block ------------------------------

void PlcHmi009FreshConfirmTest::staleFastBlockCannotConfirmM42Hold()
{
    // M42 is confirmed from the fast block, so a fast block that is not Valid
    // must not confirm the M42 hold even when it shows the requested value; a
    // Valid fast block then confirms exactly once.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    QVERIFY2(c != nullptr, "the coordinator must be constructible over the simulator");
    c->setRole(Role::Admin);
    homeReady(gw);

    gw.model().writeCoil(kM42, true);
    gw.tick();

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&results](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::Bypass)
                    results.append({ok, detail});
            });

    const ControlCoordinator::CommandResult issued = c->bypass(kM42, true);
    QVERIFY2(issued.accepted,
             qPrintable(QStringLiteral("precondition: the M42 manual command was not accepted "
                                       "through the coordinator public API (%1)")
                            .arg(issued.reason)));

    // Stale/failed fast-block evidence must not confirm.
    now += 1'000;
    c->onSnapshot(DeviceSnapshot(fastBlockSnapshot(true, DataQuality::ProtocolError, now)));
    now += 1'000;
    c->onSnapshot(DeviceSnapshot(fastBlockSnapshot(true, DataQuality::Stale, now)));
    QVERIFY2(successCount(results) == 0,
             qPrintable(QStringLiteral("a non-valid fast block confirmed the M42 hold: %1 "
                                       "success result(s); sequence: %2")
                            .arg(successCount(results))
                            .arg(describeResults(results))));

    // Fresh fast-block evidence confirms exactly once.
    c->onSnapshot(DeviceSnapshot(fastBlockSnapshot(true, DataQuality::Valid, 0)));
    QVERIFY2(successCount(results) == 1,
             qPrintable(QStringLiteral("valid fast-block evidence produced %1 success "
                                       "result(s) instead of exactly one; sequence: %2")
                            .arg(successCount(results))
                            .arg(describeResults(results))));
    QVERIFY2(!results.first().detail.trimmed().isEmpty(),
             "the confirmed M42 hold carries no visible detail");
    c->onSnapshot(DeviceSnapshot(fastBlockSnapshot(true, DataQuality::Valid, 0)));
    QCOMPARE(results.size(), 1);
    QCOMPARE(successCount(results), 1);
}

// --- OB-3: exactly the source block's freshness decides confirmation ------------
//
// Brief boundary: "M42 confirmation sourced from the fast block; M105-M111 from
// the command block (no cross-block acceptance)". The existing cases prove the
// negative side (a bad command block cannot confirm M106; a bad fast block
// cannot confirm M42). This case proves the converse isolation: the *other*
// block's evidence must neither suppress a source-block confirmation nor be
// able to produce one on its own.
//
// M42 uses fast-block evidence and the command block is deliberately kept
// Stale: if M42 leaked onto command-block gating its Valid fast block could not
// confirm. M106 uses command-block evidence and the fast block is deliberately
// kept Stale: if M106 leaked onto fast-block gating its Valid command block
// could not confirm. Each command must confirm exactly once from its own block.

void PlcHmi009FreshConfirmTest::sourceBlockEvidenceAloneConfirms()
{
    // M42: valid fast block + stale command block must still confirm.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
        QVERIFY2(c != nullptr, "the coordinator must be constructible over the simulator");
        c->setRole(Role::Admin);
        homeReady(gw);

        gw.model().writeCoil(kM42, true);
        gw.tick();

        QVector<ResultRecord> results;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&results](Command cmd, bool ok, const QString &detail) {
                    if (cmd == Command::Bypass)
                        results.append({ok, detail});
                });

        const ControlCoordinator::CommandResult issued = c->bypass(kM42, true);
        QVERIFY2(issued.accepted,
                 qPrintable(QStringLiteral("precondition: the M42 command was not accepted (%1)")
                                .arg(issued.reason)));

        DeviceSnapshotData d = fastBlockSnapshot(true, DataQuality::Valid, 0);
        d.command_quality = DataQuality::Stale; // the *other* block is stale
        d.command_age_ms = 10'000;
        c->onSnapshot(DeviceSnapshot(d));

        QVERIFY2(successCount(results) == 1,
                 qPrintable(QStringLiteral("the Valid fast block failed to confirm M42 while the "
                                           "command block was stale (%1 success result(s)); "
                                           "cross-block suppression; sequence: %2")
                                .arg(successCount(results))
                                .arg(describeResults(results))));
    }

    // M106: valid command block + stale fast block must still confirm.
    {
        SimulatedPlcGateway gw;
        gw.start();
        qint64 now = 0;
        std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
        QVERIFY2(c != nullptr, "the coordinator must be constructible over the simulator");
        c->setRole(Role::Admin);
        homeReady(gw);

        gw.model().writeCoil(kM106, true);
        gw.tick();

        QVector<ResultRecord> results;
        connect(c.get(), &ControlCoordinator::commandResult, this,
                [&results](Command cmd, bool ok, const QString &detail) {
                    if (cmd == Command::ManualCommand)
                        results.append({ok, detail});
                });

        const ControlCoordinator::CommandResult issued = c->manualHold(kM106, true);
        QVERIFY2(issued.accepted,
                 qPrintable(QStringLiteral("precondition: the manual command was not accepted (%1)")
                                .arg(issued.reason)));

        DeviceSnapshotData d = commandBlockSnapshot(true, DataQuality::Valid, 0);
        d.fast_quality = DataQuality::Stale; // the *other* block is stale
        d.fast_age_ms = 10'000;
        c->onSnapshot(DeviceSnapshot(d));

        QVERIFY2(successCount(results) == 1,
                 qPrintable(QStringLiteral("the Valid command block failed to confirm M106 while "
                                           "the fast block was stale (%1 success result(s)); "
                                           "cross-block suppression; sequence: %2")
                                .arg(successCount(results))
                                .arg(describeResults(results))));
    }
}

// --- OB-3: the existing defensive timeout still converges the command -----------

void PlcHmi009FreshConfirmTest::staleCommandBlockConvergesToNonSuccessTerminal()
{
    // A stale block that never shows fresh evidence must not stay pending
    // forever: past the 3 s defensive timeout the command converges to a
    // non-success terminal with a non-empty detail, and never reports success.
    SimulatedPlcGateway gw;
    gw.start();
    qint64 now = 0;
    std::unique_ptr<ControlCoordinator> c(makeCoordinator(gw, now));
    QVERIFY2(c != nullptr, "the coordinator must be constructible over the simulator");
    c->setRole(Role::Admin);
    homeReady(gw);

    gw.model().writeCoil(kM106, true);
    gw.tick();

    QVector<ResultRecord> results;
    connect(c.get(), &ControlCoordinator::commandResult, this,
            [&results](Command cmd, bool ok, const QString &detail) {
                if (cmd == Command::ManualCommand)
                    results.append({ok, detail});
            });

    const ControlCoordinator::CommandResult issued = c->manualHold(kM106, true);
    QVERIFY2(issued.accepted,
             qPrintable(QStringLiteral("precondition: the manual command was not accepted (%1)")
                            .arg(issued.reason)));

    now += 1'000;
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Stale, now)));
    now += 1'000;
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Stale, now)));
    QVERIFY2(results.isEmpty(),
             qPrintable(QStringLiteral("the command terminated before its confirmation timeout; "
                                       "sequence: %1")
                            .arg(describeResults(results))));

    now += kConfirmTimeoutMs; // past the defensive timeout
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Stale, now)));

    QVERIFY2(successCount(results) == 0,
             qPrintable(QStringLiteral("an unconfirmable stale command reported success; "
                                       "sequence: %1")
                            .arg(describeResults(results))));
    QVERIFY2(anyNonSuccessTerminalWithDetail(results),
             qPrintable(QStringLiteral("the unconfirmable command did not converge to a "
                                       "non-success terminal with a non-empty detail; "
                                       "sequence: %1")
                            .arg(describeResults(results))));
    QCOMPARE(results.size(), 1); // exactly one terminal outcome

    // Later stale snapshots must not produce a second terminal.
    now += 1'000;
    c->onSnapshot(DeviceSnapshot(commandBlockSnapshot(true, DataQuality::Stale, now)));
    QCOMPARE(results.size(), 1);
}

QTEST_GUILESS_MAIN(PlcHmi009FreshConfirmTest)
#include "plc_hmi_009_fresh_confirm_test.moc"
