// PLC-HMI-009 R2 developer-owned regression tests (D4): logout-clear lifecycle
// (D1) and the command-confirmation freshness guards (D3).
//
// Rig convention mirrors tests/unit/test_control_coordinator.cpp: a
// ControlCoordinator over a recording fake PulseTransport; terminal outcomes
// are driven through onSubmissionCompleted with the request identity returned
// by the fake; confirmations are synthetic snapshots with explicit per-block
// quality/age so the freshness guards are exercised deterministically.

#include <QtTest>

#include "application/control_coordinator.h"
#include "domain/quality.h"

using namespace hlm;

namespace {

constexpr quint16 kM42 = 42;
constexpr quint16 kM106 = 106;

constexpr quint64 kGeneration = 9;

struct RecordedWrite {
    quint16 address = 0;
    bool value = false;
    quint64 request_id = 0;
};

// Deterministic submission recorder. Every accepted write gets a unique
// request id and the fixed gateway generation; the test delivers the
// correlated completion with the recorded identity.
class FakeTransport
{
public:
    ControlCoordinator::PulseTransport make()
    {
        ControlCoordinator::PulseTransport t;
        t.startPulse = [this](quint16 address) -> SubmissionResult {
            return record(address, true);
        };
        t.writeHold = [this](quint16 address, bool value) -> SubmissionResult {
            return record(address, value);
        };
        t.writeCoil = [this](quint16 address, bool value, CommandPriority)
            -> SubmissionResult {
            return record(address, value);
        };
        t.writeRegister = [this](quint16, quint16, CommandPriority)
            -> SubmissionResult {
            return accept();
        };
        return t;
    }

    void rejectWrites(const QString &reason) { m_rejectReason = reason; }

    SubmissionCompletion completionFor(const RecordedWrite &w, bool ok) const
    {
        SubmissionCompletion c;
        c.request_id = w.request_id;
        c.gateway_generation = kGeneration;
        c.operation = PlcOperation::WriteCoil;
        c.address = w.address;
        c.result = ok;
        if (!ok)
            c.error = QStringLiteral("write failed");
        return c;
    }

    QVector<RecordedWrite> writes;

private:
    SubmissionResult accept()
    {
        SubmissionResult r;
        r.accepted = true;
        r.request_id = ++m_nextId;
        r.gateway_generation = kGeneration;
        return r;
    }

    SubmissionResult record(quint16 address, bool value)
    {
        if (!m_rejectReason.isEmpty()) {
            SubmissionResult r;
            r.accepted = false;
            r.gateway_generation = kGeneration;
            r.immediate_rejection_reason = m_rejectReason;
            return r;
        }
        const SubmissionResult r = accept();
        writes.append({address, value, r.request_id});
        return r;
    }

    quint64 m_nextId = 0;
    QString m_rejectReason;
};

struct ResultLog {
    int successes = 0;
    int failures = 0;
    QString lastDetail;

    int total() const { return successes + failures; }
};

void observeResults(ControlCoordinator &c, Command cmd, ResultLog &log)
{
    QObject::connect(&c, &ControlCoordinator::commandResult, &c,
                     [&log, cmd](Command reported, bool ok, const QString &detail) {
                         if (reported != cmd)
                             return;
                         if (ok)
                             ++log.successes;
                         else
                             ++log.failures;
                         log.lastDetail = detail;
                     });
}

void observeStarts(ControlCoordinator &c, Command cmd, int &starts)
{
    QObject::connect(&c, &ControlCoordinator::commandPending, &c,
                     [&starts, cmd](Command reported) {
                         if (reported == cmd)
                             ++starts;
                     });
    QObject::connect(&c, &ControlCoordinator::commandAccepted, &c,
                     [&starts, cmd](Command reported) {
                         if (reported == cmd)
                             ++starts;
                     });
}

// Synthetic snapshot: online, manual mode (M1), homed (M61). The command
// block carries M106 when requested; quality/age are explicit so the
// freshness guard is observable.
DeviceSnapshot manualSnapshot(bool m106, DataQuality commandQuality,
                              qint64 commandAge)
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (1u << 1) | (1u << 9); // M1 manual mode, M61 homed
    d.commandBits = m106 ? quint16(0x0040) : quint16(0); // M106
    d.command_quality = commandQuality;
    d.command_age_ms = commandAge;
    d.fast_quality = DataQuality::Valid;
    d.fast_age_ms = 0;
    return DeviceSnapshot(d);
}

// Synthetic snapshot carrying M42 (status word 3 bit 12) with explicit fast
// block quality/age.
DeviceSnapshot m42Snapshot(DataQuality fastQuality, qint64 fastAge)
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord3 = (1u << 12); // M42
    d.fast_quality = fastQuality;
    d.fast_age_ms = fastAge;
    d.command_quality = DataQuality::Valid;
    d.command_age_ms = 0;
    return DeviceSnapshot(d);
}

} // namespace

class PlcHmi009DeveloperRegressionTest : public QObject
{
    Q_OBJECT

private slots:
    // --- D1: logout-clear lifecycle -----------------------------------------
    void logoutClearIsNotConfirmedBeforeAllCompletions();
    void logoutClearFailedCompletionConvergesToExactlyOneFailure();
    void logoutClearRejectingTransportConvergesImmediatelyToFailure();
    void logoutClearAbsentTransportConvergesImmediatelyToFailure();
    void logoutClearDuplicateWhilePendingIsAbsorbed();

    // --- D3: command confirmation freshness ---------------------------------
    void staleCommandQualityDoesNotConfirmManualCommand();
    void freshCommandQualityConfirmsManualCommand();
    void validCommandQualityWithOldCommandAgeDoesNotConfirm();
    void freshFastQualityConfirmsBypassM42();
    void oldFastAgeDoesNotConfirmBypassM42();
};

void PlcHmi009DeveloperRegressionTest::logoutClearIsNotConfirmedBeforeAllCompletions()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    observeResults(c, Command::LogoutClear, log);
    int starts = 0;
    observeStarts(c, Command::LogoutClear, starts);

    c.logoutClear();

    // Visible request-start, all eight writes submitted, nothing confirmed.
    QCOMPARE(ft.writes.size(), 8);
    QCOMPARE(ft.writes[0].address, kM42);
    QCOMPARE(ft.writes[6].address, quint16(111));
    // The M50=0 home-start clear is appended LAST (PLC-HMI-011 D7).
    QCOMPARE(ft.writes[7].address, quint16(50));
    QCOMPARE(ft.writes[7].value, false);
    QCOMPARE(starts, 2); // one commandAccepted + one commandPending
    QCOMPARE(log.total(), 0); // acceptance is not confirmation

    // Seven of eight completions: still no terminal.
    for (int i = 0; i < 7; ++i)
        c.onSubmissionCompleted(ft.completionFor(ft.writes[i], true));
    QCOMPARE(log.total(), 0);

    // The eighth completes the clear: exactly one visible success terminal.
    c.onSubmissionCompleted(ft.completionFor(ft.writes[7], true));
    QCOMPARE(log.successes, 1);
    QCOMPARE(log.failures, 0);
    QVERIFY(!log.lastDetail.isEmpty());

    // Late/duplicate completion is ignored: no second terminal.
    c.onSubmissionCompleted(ft.completionFor(ft.writes[0], true));
    QCOMPARE(log.total(), 1);
    QCOMPARE(starts, 2);
}

void PlcHmi009DeveloperRegressionTest::logoutClearFailedCompletionConvergesToExactlyOneFailure()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    observeResults(c, Command::LogoutClear, log);

    c.logoutClear();
    QCOMPARE(ft.writes.size(), 8);

    // One failed completion converges the whole clear to failure.
    c.onSubmissionCompleted(ft.completionFor(ft.writes[0], false));
    QCOMPARE(log.failures, 1);
    QCOMPARE(log.successes, 0);
    QVERIFY(!log.lastDetail.isEmpty());

    // The remaining successful completions must not resurrect a success.
    for (int i = 1; i < 8; ++i)
        c.onSubmissionCompleted(ft.completionFor(ft.writes[i], true));
    QCOMPARE(log.total(), 1);
    QCOMPARE(log.successes, 0);
}

void PlcHmi009DeveloperRegressionTest::logoutClearRejectingTransportConvergesImmediatelyToFailure()
{
    FakeTransport ft;
    ft.rejectWrites(QStringLiteral("transport rejected write"));
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    observeResults(c, Command::LogoutClear, log);

    c.logoutClear();

    QCOMPARE(ft.writes.size(), 0); // nothing reached the transport
    QCOMPARE(log.failures, 1);
    QCOMPARE(log.successes, 0);
    QVERIFY(!log.lastDetail.isEmpty());

    // An unrelated late completion cannot flip the converged failure.
    SubmissionCompletion late;
    late.request_id = 1234;
    late.gateway_generation = 1;
    late.operation = PlcOperation::WriteCoil;
    late.address = kM42;
    late.result = true;
    c.onSubmissionCompleted(late);
    QCOMPARE(log.total(), 1);
    QCOMPARE(log.successes, 0);
}

void PlcHmi009DeveloperRegressionTest::logoutClearAbsentTransportConvergesImmediatelyToFailure()
{
    ControlCoordinator::PulseTransport absent; // every callback null
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(absent, {}, [&now] { return now; });
    observeResults(c, Command::LogoutClear, log);

    c.logoutClear();

    QCOMPARE(log.failures, 1);
    QCOMPARE(log.successes, 0);
    QVERIFY(!log.lastDetail.isEmpty());
    QVERIFY(!c.logoutClearPending());
}

void PlcHmi009DeveloperRegressionTest::logoutClearDuplicateWhilePendingIsAbsorbed()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    observeResults(c, Command::LogoutClear, log);
    int starts = 0;
    observeStarts(c, Command::LogoutClear, starts);

    c.logoutClear();
    QCOMPARE(ft.writes.size(), 8);
    QCOMPARE(starts, 2);
    QCOMPARE(log.total(), 0);

    // Duplicate while pending: no second generation, writes or request-start.
    c.logoutClear();
    QCOMPARE(ft.writes.size(), 8);
    QCOMPARE(starts, 2);
    QCOMPARE(log.total(), 0);
    QVERIFY(c.logoutClearPending());

    for (const RecordedWrite &w : ft.writes)
        c.onSubmissionCompleted(ft.completionFor(w, true));

    // Exactly one terminal for the whole lifecycle.
    QCOMPARE(log.successes, 1);
    QCOMPARE(log.failures, 0);
    QVERIFY(!c.logoutClearPending());
}

void PlcHmi009DeveloperRegressionTest::staleCommandQualityDoesNotConfirmManualCommand()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    c.setRole(Role::Admin);
    c.onSnapshot(manualSnapshot(false, DataQuality::Valid, 0)); // online + manual + homed
    observeResults(c, Command::ManualCommand, log);

    QVERIFY(c.manualHold(kM106, true).accepted);
    QCOMPARE(ft.writes.size(), 1);

    // The command block shows the requested M106 value but is stale: no confirm.
    c.onSnapshot(manualSnapshot(true, DataQuality::Stale, 0));
    QCOMPARE(log.total(), 0);

    // Defensive timeout converges it to exactly one visible failure.
    now += 3001; // past kManualConfirmTimeoutMs
    c.onSnapshot(manualSnapshot(true, DataQuality::Stale, 0));
    QCOMPARE(log.failures, 1);
    QCOMPARE(log.successes, 0);
    QVERIFY(!log.lastDetail.isEmpty());
}

void PlcHmi009DeveloperRegressionTest::freshCommandQualityConfirmsManualCommand()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    c.setRole(Role::Admin);
    c.onSnapshot(manualSnapshot(false, DataQuality::Valid, 0));
    observeResults(c, Command::ManualCommand, log);

    QVERIFY(c.manualHold(kM106, true).accepted);
    QVERIFY(c.manualHold(kM106, true).accepted == false); // duplicate pending rejected

    c.onSnapshot(manualSnapshot(true, DataQuality::Valid, 0));
    QCOMPARE(log.successes, 1);
    QCOMPARE(log.failures, 0);
    QVERIFY(!log.lastDetail.isEmpty());

    // No re-report on a later snapshot.
    c.onSnapshot(manualSnapshot(true, DataQuality::Valid, 0));
    QCOMPARE(log.total(), 1);
}

void PlcHmi009DeveloperRegressionTest::validCommandQualityWithOldCommandAgeDoesNotConfirm()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    c.setRole(Role::Admin);
    c.onSnapshot(manualSnapshot(false, DataQuality::Valid, 0));
    observeResults(c, Command::ManualCommand, log);

    QVERIFY(c.manualHold(kM106, true).accepted);

    // Command quality Valid is not enough: the command block age must be
    // within the approved threshold (D3 alignment).
    c.onSnapshot(manualSnapshot(true, DataQuality::Valid, kCommandStaleMs + 1));
    QCOMPARE(log.total(), 0);

    now += 3001;
    c.onSnapshot(manualSnapshot(true, DataQuality::Valid, kCommandStaleMs + 1));
    QCOMPARE(log.failures, 1);
    QCOMPARE(log.successes, 0);
    QVERIFY(!log.lastDetail.isEmpty());
}

void PlcHmi009DeveloperRegressionTest::freshFastQualityConfirmsBypassM42()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    c.setRole(Role::Admin);
    c.onSnapshot(m42Snapshot(DataQuality::Valid, 0)); // online
    observeResults(c, Command::Bypass, log);

    QVERIFY(c.bypass(kM42, true).accepted);
    QCOMPARE(ft.writes.size(), 1);
    QCOMPARE(ft.writes[0].address, kM42);

    // Valid fast block within the fast threshold confirms M42.
    c.onSnapshot(m42Snapshot(DataQuality::Valid, kFastStaleMs));
    QCOMPARE(log.successes, 1);
    QCOMPARE(log.failures, 0);
}

void PlcHmi009DeveloperRegressionTest::oldFastAgeDoesNotConfirmBypassM42()
{
    FakeTransport ft;
    qint64 now = 1000;
    ResultLog log;
    ControlCoordinator c(ft.make(), {}, [&now] { return now; });
    c.setRole(Role::Admin);
    c.onSnapshot(m42Snapshot(DataQuality::Valid, 0));
    observeResults(c, Command::Bypass, log);

    QVERIFY(c.bypass(kM42, true).accepted);

    // Valid fast quality with an age past the fast threshold: no confirm.
    c.onSnapshot(m42Snapshot(DataQuality::Valid, kFastStaleMs + 1));
    QCOMPARE(log.total(), 0);

    now += 3001;
    c.onSnapshot(m42Snapshot(DataQuality::Valid, kFastStaleMs + 1));
    QCOMPARE(log.failures, 1);
    QCOMPARE(log.successes, 0);
    QVERIFY(!log.lastDetail.isEmpty());
}

QTEST_GUILESS_MAIN(PlcHmi009DeveloperRegressionTest)
#include "plc_hmi_009_developer_regression_test.moc"
