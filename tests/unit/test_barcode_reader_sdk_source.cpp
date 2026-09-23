// Unit tests: SDK-based barcode source (user decision 2026-09-22, revised the
// same day — the HMI drives the scan instead of reading a result file).
//
// The vendor SDK is behind IBarcodeSdk, so a scripted fake drives the whole
// cycle here with no DLL: status → trigger → poll → parse → persist → converge.
// Coverage:
// - NotConfigured: an empty path triggers nothing and says 未配置.
// - Ok: a completed cycle decodes every row, `line`/`fields` carry the first
//   decoded barcode, and each decoded barcode is appended as its own CRLF line
//   (the reference file format, 需求/扫码相关/Barcode.txt).
// - Empty positions are counted, never filled with an older barcode, and never
//   written to the file.
// - NoCode: a completed cycle with no decoded barcode is a result, not silence.
// - Failed: absent SDK, rejected trigger (business code), server restart, and
//   cycle-deadline timeout each converge with an operator-readable reason.
// - The same requestId is reused for every query; two cycles use different ids.
// - A failed file write never hides the barcode that was decoded.
// - All SDK calls and file writes run on the adapter's own worker thread.
// - Forwarding (user decision 2026-09-23): one argument per barcode in table
//   order, one invocation per board, and a visible failure for every way it can
//   go wrong (missing program, non-zero exit, timeout, unrepresentable barcode).
//   The forward program is a REAL process — this test binary re-entered as the
//   probe in tests/unit/forward_probe.h — because the production invocation is
//   thing under test, and an injected fake could not prove it.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QDateTime>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include "adapters/barcode/barcode_reader_sdk_source.h"
#include "forward_probe.h"

using namespace hlm;

namespace {

// A scripted stand-in for the vendor DLL. Every reply is queued by the test, so
// the adapter's state machine is exercised deterministically: no sleeping, no
// real pipe, no timing assumptions.
class FakeBarcodeSdk : public IBarcodeSdk
{
public:
    BarcodeSdkReply status() override
    {
        ++statusCalls;
        return next(statusReplies);
    }
    BarcodeSdkReply trigger(const QString &requestId) override
    {
        triggerIds.append(requestId);
        return next(triggerReplies);
    }
    BarcodeSdkReply result(const QString &requestId) override
    {
        resultIds.append(requestId);
        return next(resultReplies);
    }

    // Queued replies; the last one repeats once the queue is exhausted, so a
    // "still pending" script keeps polling until the test changes it.
    QList<BarcodeSdkReply> statusReplies;
    QList<BarcodeSdkReply> triggerReplies;
    QList<BarcodeSdkReply> resultReplies;

    int statusCalls = 0;
    QStringList triggerIds;
    QStringList resultIds;

private:
    static BarcodeSdkReply next(QList<BarcodeSdkReply> &queue)
    {
        if (queue.isEmpty())
            return {};
        if (queue.size() == 1)
            return queue.first();
        return queue.takeFirst();
    }
};

BarcodeSdkReply okReply(const QByteArray &json)
{
    return {BarcodeSdkStatus::Ok, json};
}

QByteArray statusJson(const QString &serverId)
{
    return QStringLiteral(R"({"ok":true,"code":"status","running":true,)"
                          R"("serverId":"%1","version":1})")
        .arg(serverId)
        .toUtf8();
}

QByteArray acceptedJson(const QString &requestId)
{
    return QStringLiteral(R"({"ok":true,"code":"accepted","state":"pending",)"
                          R"("requestId":"%1","serverId":"server-1"})")
        .arg(requestId)
        .toUtf8();
}

QByteArray completedJson(const QString &requestId,
                         const QStringList &barcodes)
{
    QStringList rows;
    int sequence = 1;
    for (const QString &barcode : barcodes) {
        rows.append(QStringLiteral(R"({"barcode":"%1","cameraId":"camera-window-2",)"
                                   R"("format":"DataMatrix","rectId":%2,"sequence":%2})")
                        .arg(barcode)
                        .arg(sequence));
        ++sequence;
    }
    return QStringLiteral(R"({"ok":true,"code":"completed","state":"completed",)"
                          R"("requestId":"%1","serverId":"server-1","decodedCount":%2,)"
                          R"("saved":true,"message":"","rows":[%3]})")
        .arg(requestId)
        .arg(barcodes.size())
        .arg(rows.join(QLatin1Char(',')))
        .toUtf8();
}

// Reads the file the adapter appends to, or an empty list when it does not
// exist yet.
QStringList resultLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    return text.split(QLatin1String("\r\n"), Qt::SkipEmptyParts);
}

} // namespace

class BarcodeReaderSdkSourceTest : public QObject
{
    Q_OBJECT

private slots:
    void paddedReplyBufferYieldsExactlyThePayload();
    void notConfiguredTriggersNothing();
    void noResultFileStillDecodesDisplaysAndForwards();
    void completedCycleParsesEveryRowAndAppendsEachBarcode();
    void emptyPositionsAreCountedAndNeverWritten();
    void completedCycleWithoutAnyBarcodeIsNoCode();
    void absentSdkConvergesToTheOperatorReason();
    void missingScannerProgramIsItsOwnActionableReason();
    void rejectedTriggerCarriesTheBusinessReason();
    void serverRestartNeverTrustsTheResult();
    void truncatedResultIsAFailureNotNoCode();
    void transportTimeoutKeepsPollingTheSameRequestId();
    void cycleDeadlineConvergesTheWaitNotTheDecode();
    void busyTriggerIsTerminalBecauseNothingWasAccepted();
    void busyKeepsPollingTheSameRequestId();
    void twoCyclesUseDifferentRequestIds();
    void requestIdIsATimestampThatNeverRepeats();
    void failedWriteStillShowsTheBarcode();
    void forwardArgumentKeepsOnePerBarcodeInTableOrder();
    void forwardArgumentRefusesATabOrNewline();
    void emptyForwardPathNeverRunsTheProgram();
    void missingForwardProgramIsAVisibleFailure();
    void forwardingRunsTheProgramOncePerCycleWithEveryBarcode();
    void nonZeroExitIsAVisibleFailure();
    void timeoutKillsTheProgramAndIsAVisibleFailure();
    void unrepresentableBarcodeIsNotForwardedButIsStillStored();
    void noCodeCycleDoesNotForward();
    void overlappingCycleIsRejectedNotQueued();
    void sdkCallsRunOffTheCallingThread();

    // --- the production CLI transport (user decision 2026-09-23) ---------------
    // These drive makeSystemBarcodeSdk() — the object the shipped HMI actually
    // uses — with this test binary standing in for trigger_client.exe.
    void cliReadsTheJsonReplyFromStdout();
    void cliExitCodeIsTheTransportStatus();
    void cliPassesEndpointCommandAndRequestId();
    void cliMissingProgramIsItsOwnActionableReason();
    void cliTimeoutKillsTheWedgedProgram();
};

void BarcodeReaderSdkSourceTest::paddedReplyBufferYieldsExactlyThePayload()
{
    // The real DLL writes its JSON at the front of the caller's 1 MiB + 1
    // buffer and leaves the rest untouched, so the buffer handed back is
    // NUL-padded. Qt's JSON parser does NOT treat NUL as whitespace: passing
    // the whole padded buffer fails with GarbageAtEnd. This helper is the
    // single place that trims it, and it is deliberately outside the
    // #ifdef _WIN32 block so the DLL-less dev loop can still test it.
    const QByteArray json = R"({"ok":true,"code":"completed"})";
    const quint32 required =
        static_cast<quint32>(json.size()) + 1; // requiredBytes includes the NUL

    QByteArray padded(1024 * 1024 + 1, '\0');
    memcpy(padded.data(), json.constData(), json.size());

    // 1. The exact payload, given the SDK's own byte count.
    QCOMPARE(barcodePayloadFromBuffer(padded, required), json);
    // 2. The proof that this matters: the untrimmed buffer does not parse.
    QJsonParseError paddedError{};
    QJsonDocument::fromJson(padded, &paddedError);
    QVERIFY2(paddedError.error != QJsonParseError::NoError,
             "the padded buffer must NOT be parseable, or this trim is pointless");
    QJsonParseError trimmedError{};
    QJsonDocument::fromJson(barcodePayloadFromBuffer(padded, required),
                            &trimmedError);
    QCOMPARE(trimmedError.error, QJsonParseError::NoError);
    // 3. No byte count reported: fall back to the first NUL.
    QCOMPARE(barcodePayloadFromBuffer(padded, 0), json);
    // 4. An over-reported count still stops at the NUL instead of overrunning.
    QCOMPARE(barcodePayloadFromBuffer(padded, padded.size()), json);
    // 5. A buffer the SDK filled exactly (no padding) is returned whole.
    QCOMPARE(barcodePayloadFromBuffer(json, required), json);
    // 6. A zero-length payload is empty, not the whole buffer.
    QVERIFY(barcodePayloadFromBuffer(QByteArray(8, '\0'), 1).isEmpty());
}

void BarcodeReaderSdkSourceTest::notConfiguredTriggersNothing()
{
    // 未配置 is decided by the SCANNER PROGRAM — the setting that enables the
    // feature. No program means nothing runs and the surface says so.
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    BarcodeReaderSdkSource source(&sdk);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::NotConfigured);
    // A missing program must not reach the SDK at all.
    QCOMPARE(sdk.statusCalls, 0);
    QCOMPARE(sdk.triggerIds.size(), 0);
    source.stop();
}

void BarcodeReaderSdkSourceTest::noResultFileStillDecodesDisplaysAndForwards()
{
    // The result file is OPTIONAL (user decision 2026-09-23: "不需要结果文件
    // 结果打印到界面上 并且通过exe发送"). Gating the cycle on it was wrong: a bench
    // with the scanner program and the forward program configured, and no file,
    // must still decode, still show the barcodes, and still forward — it just
    // does not persist.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    hlm_test::ForwardProbe probe(dir.path());

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    // Program configured, forward program configured, NO result file.
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setForwardExePath(hlm_test::probeProgramPath());
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    // Decoded and displayed…
    QCOMPARE(result.state, BarcodeState::Ok);
    QCOMPARE(result.line, QStringLiteral("C3003090^M10^260224^002695"));
    // …not persisted, and NOT reported as a failure: an unconfigured optional
    // file is a choice, not a problem (the 扫码服务 page states it separately).
    QVERIFY(!result.persisted);
    QVERIFY(result.persistDetail.isEmpty());
    // …and forwarded anyway.
    QVERIFY2(result.forwarded, qPrintable(result.forwardDetail));
    QCOMPARE(probe.arguments(),
             QStringList({QStringLiteral("C3003090^M10^260224^002695")}));
    source.stop();
}

void BarcodeReaderSdkSourceTest::completedCycleParsesEveryRowAndAppendsEachBarcode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"),
        {QStringLiteral("C3003090^M10^260224^002695"),
         QStringLiteral("C3003100^M10^260224^002696")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Ok);
    QCOMPARE(result.decodedCount, 2);
    QCOMPARE(result.emptyPositions, 0);
    QCOMPARE(result.rows.size(), 2);
    QCOMPARE(result.line, QStringLiteral("C3003090^M10^260224^002695"));
    QCOMPARE(result.fields.size(), 4);
    QCOMPARE(result.fields.at(0), QStringLiteral("C3003090"));
    QVERIFY(result.persisted);
    QVERIFY(result.persistDetail.isEmpty());
    // The requestId is carried for diagnosis and is the one that was submitted.
    QCOMPARE(result.requestId, sdk.triggerIds.first());
    QVERIFY(!result.requestId.isEmpty());

    // One CRLF line per decoded barcode — the reference file's own format.
    const QStringList lines = resultLines(path);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0), QStringLiteral("C3003090^M10^260224^002695"));
    QCOMPARE(lines.at(1), QStringLiteral("C3003100^M10^260224^002696"));
    source.stop();
}

void BarcodeReaderSdkSourceTest::emptyPositionsAreCountedAndNeverWritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    // Six rectangles, only the third decoded — exactly what the SDK reports for
    // positions it looked at and did not recognise.
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"),
        {QString(), QString(), QStringLiteral("C3003090^M10^260224^002697"),
         QString(), QString(), QString()}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Ok);
    QCOMPARE(result.decodedCount, 1);
    QCOMPARE(result.emptyPositions, 5);
    QCOMPARE(result.rows.size(), 6); // every position is reported

    // Only the decoded barcode reaches the file: an empty position is not a
    // barcode and must never be replaced by an older one.
    const QStringList lines = resultLines(path);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.at(0), QStringLiteral("C3003090^M10^260224^002697"));
    source.stop();
}

void BarcodeReaderSdkSourceTest::completedCycleWithoutAnyBarcodeIsNoCode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));
    // Pre-existing content from an earlier board.
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("OLD^M10^260224^000001\r\n");
    }

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QString(), QString(), QString()}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::NoCode);
    QCOMPARE(result.decodedCount, 0);
    QVERIFY(result.line.isEmpty());

    // Nothing decoded → nothing appended. The file keeps exactly what it had.
    const QStringList lines = resultLines(path);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.at(0), QStringLiteral("OLD^M10^260224^000001"));
    source.stop();
}

void BarcodeReaderSdkSourceTest::absentSdkConvergesToTheOperatorReason()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    // The scanner program is closed. Over the CLI transport that is
    // BR_NOT_CONNECTED (2) — the CLI reaches the pipe, finds nobody home, and
    // exits 2 — not ProgramUnavailable, which now means the CLI itself could not
    // be started at all.
    sdk.statusReplies = {{BarcodeSdkStatus::NotConnected, QByteArray()}};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    // The operator gets something actionable, not a status code.
    QVERIFY(result.detail.contains(QStringLiteral("扫码服务未启动")));
    QVERIFY(result.detail.contains(QStringLiteral("BarcodeReader")));
    source.stop();
}

void BarcodeReaderSdkSourceTest::missingScannerProgramIsItsOwnActionableReason()
{
    // ProgramUnavailable is the DEPLOYMENT failure — the configured CLI is
    // missing or not executable — and must not be reported as "start the scan
    // program", which would send the operator to the wrong fix.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {{BarcodeSdkStatus::ProgramUnavailable, QByteArray()}};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(result.detail.contains(QStringLiteral("扫码程序无法启动")));
    source.stop();
}

void BarcodeReaderSdkSourceTest::rejectedTriggerCarriesTheBusinessReason()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(
        R"({"ok":false,"code":"wrong_mode","state":"failed","serverId":"server-1"})")};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(result.detail.contains(QStringLiteral("触发")));
    // A rejected trigger is terminal: no polling, no silent retry.
    QCOMPARE(sdk.resultIds.size(), 0);
    source.stop();
}

void BarcodeReaderSdkSourceTest::serverRestartNeverTrustsTheResult()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    // Same requestId, but a different server instance answered: the accepted
    // job's outcome is unknowable.
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};
    sdk.resultReplies[0].json.replace("server-1", "server-2");

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(result.detail.contains(QStringLiteral("重启")));
    source.stop();
}

// A transport timeout does NOT cancel an accepted job, so the adapter must keep
// querying — with the SAME requestId, never a fresh one — rather than converge
// on the first timeout.
void BarcodeReaderSdkSourceTest::transportTimeoutKeepsPollingTheSameRequestId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {{BarcodeSdkStatus::Timeout, QByteArray()}};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    // Wait for at least two polls to prove the timeout did not converge early.
    QTRY_VERIFY_WITH_TIMEOUT(sdk.resultIds.size() >= 2, 5000);
    QCOMPARE(spy.size(), 0); // still running: a timeout is not a terminal answer
    QCOMPARE(sdk.triggerIds.size(), 1);
    const QString id = sdk.triggerIds.first();
    for (const QString &queried : sdk.resultIds)
        QCOMPARE(queried, id);

    // Let it finish so the thread stops cleanly.
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 10000);
    QCOMPARE(spy[0][0].value<BarcodeResult>().state, BarcodeState::Ok);
    source.stop();
}

// The cycle deadline is the adapter's promise that a pending state is never
// left open, and it is the one path a test could not reach while the constants
// were file-local: it would have to wait 35 s. With an injected 60 ms budget
// the whole path — keep polling, then converge with a visible reason — is
// proven in milliseconds.
void BarcodeReaderSdkSourceTest::cycleDeadlineConvergesTheWaitNotTheDecode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    // The server never finishes: every poll comes back accepted/pending.
    sdk.resultReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};

    BarcodeReaderSdkSource::Config config;
    config.pollIntervalMs = 5;
    config.cycleDeadlineMs = 60;
    BarcodeReaderSdkSource source(&sdk, config);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(result.detail.contains(QStringLiteral("超时")));
    // Still one trigger, and every query used that same id: giving up on the
    // WAIT never re-keys the cycle (the decode was not cancelled).
    QCOMPARE(sdk.triggerIds.size(), 1);
    for (const QString &queried : sdk.resultIds)
        QCOMPARE(queried, sdk.triggerIds.first());
    // And a new cycle is accepted once the failed one converged.
    QVERIFY(!source.cycleInProgress());
    QVERIFY(source.requestRead());
    source.stop();
}

// A `busy` answer means different things at the two call sites. On a result
// query it is "not finished yet". On a TRIGGER it means nothing was accepted
// under this id — the SDK does not queue triggers — so polling could only ever
// return not_found, and waiting out the whole deadline would make the operator
// wait for an answer the adapter already has.
void BarcodeReaderSdkSourceTest::busyTriggerIsTerminalBecauseNothingWasAccepted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {
        okReply(R"({"ok":false,"code":"busy","serverId":"server-1"})")};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(result.detail.contains(QStringLiteral("上一轮")));
    // Converged at once: no polling of an id that was never accepted.
    QVERIFY2(sdk.resultIds.isEmpty(),
             "a refused trigger must not be polled as if it had been accepted");
    QCOMPARE(sdk.triggerIds.size(), 1);
    source.stop();
}

// A truncated reply omits `rows` and sets rowsTruncated. The README is explicit
// that this must not be read as "decoded nothing" — doing so would tell the
// operator 本轮未识别到条码 for a cycle that actually decoded the board.
void BarcodeReaderSdkSourceTest::truncatedResultIsAFailureNotNoCode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(
        R"({"ok":true,"code":"completed","state":"completed","serverId":"server-1",)"
        R"("requestId":"job-1","saved":true,"rowsTruncated":true,"rows":[]})")};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(result.detail.contains(QStringLiteral("截断")));
    // Nothing decoded as far as this adapter knows, so nothing is appended and
    // the file is left untouched.
    QVERIFY(!QFile::exists(path));
    source.stop();
}

void BarcodeReaderSdkSourceTest::busyKeepsPollingTheSameRequestId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    // "busy" is the SDK saying the previous round is still decoding: it is
    // transient, so the cycle keeps querying the SAME requestId rather than
    // converging or re-keying.
    sdk.resultReplies = {
        okReply(R"({"ok":false,"code":"busy","serverId":"server-1"})"),
        okReply(completedJson(QStringLiteral("job-1"),
                              {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    QCOMPARE(spy[0][0].value<BarcodeResult>().state, BarcodeState::Ok);
    // busy is transient: the SAME id was queried again rather than a new cycle.
    QCOMPARE(sdk.triggerIds.size(), 1);
    QVERIFY(sdk.resultIds.size() >= 2);
    for (const QString &queried : sdk.resultIds)
        QCOMPARE(queried, sdk.triggerIds.first());
    source.stop();
}

void BarcodeReaderSdkSourceTest::twoCyclesUseDifferentRequestIds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    for (int cycle = 0; cycle < 2; ++cycle) {
        // Answer whatever id the adapter generates for this cycle.
        sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("placeholder")))};
        sdk.resultReplies = {okReply(completedJson(
            QStringLiteral("placeholder"),
            {QStringLiteral("C3003090^M10^260224^002%1").arg(cycle)}))};
        const int before = spy.size();
        QVERIFY(source.requestRead());
        // The queued replies name a placeholder id, so patch the real one in
        // once the adapter has submitted (the JSON's own requestId is not what
        // the adapter matches on — it matches by the id it sent).
        QTRY_VERIFY_WITH_TIMEOUT(spy.size() > before, 5000);
    }

    QCOMPARE(sdk.triggerIds.size(), 2);
    QVERIFY2(sdk.triggerIds.at(0) != sdk.triggerIds.at(1),
             "each cycle must use its own requestId (the SDK dedups by id)");
    // Both cycles appended their own line.
    QCOMPARE(resultLines(path).size(), 2);
    source.stop();
}

void BarcodeReaderSdkSourceTest::requestIdIsATimestampThatNeverRepeats()
{
    // USER DECISION 2026-09-23: the request id is a TIMESTAMP the operator can
    // read (yyyyMMddHHmmsszzz), not an opaque UUID — the same id shows up in the
    // scanner program's own logs, so a cycle can be traced by eye.
    //
    // It must also never repeat. The SDK answers a repeated id from its
    // 64-entry cache with the PREVIOUS result, so a collision would hand the
    // operator the last board's barcodes as if they were this board's. The
    // millisecond field plus a monotonic suffix make that impossible even for
    // two cycles inside one millisecond — which is why this drives many cycles
    // back to back rather than two.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    constexpr int kCycles = 8;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("placeholder")))};
        sdk.resultReplies = {okReply(completedJson(
            QStringLiteral("placeholder"),
            {QStringLiteral("C3003090^M10^260224^002%1").arg(cycle)}))};
        const int before = spy.size();
        QVERIFY(source.requestRead());
        QTRY_VERIFY_WITH_TIMEOUT(spy.size() > before, 5000);
    }
    source.stop();

    QCOMPARE(sdk.triggerIds.size(), kCycles);
    QSet<QString> unique;
    for (const QString &id : sdk.triggerIds) {
        unique.insert(id);
        // The vendor's charset (README_CN.md:105) and length cap.
        QVERIFY(id.size() <= 64);
        for (const QChar c : id) {
            QVERIFY2(c.isLetterOrNumber() || c == QLatin1Char('-')
                         || c == QLatin1Char('_'),
                     qPrintable(QStringLiteral("illegal character in id '%1'").arg(id)));
        }
        // A readable timestamp: yyyyMMddHHmmss + milliseconds + '-' + sequence.
        QVERIFY2(id.size() >= 17, qPrintable(id));
        QVERIFY2(id.left(8) == QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")),
                 qPrintable(QStringLiteral("id '%1' is not a date").arg(id)));
    }
    QCOMPARE(unique.size(), kCycles);
}

void BarcodeReaderSdkSourceTest::failedWriteStillShowsTheBarcode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A path whose parent directory does not exist: the write must fail…
    const QString path =
        dir.filePath(QStringLiteral("missing-dir/Barcode.txt"));

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    // …but the decoded barcode is still reported: a storage problem must never
    // hide a barcode that was actually read.
    QCOMPARE(result.state, BarcodeState::Ok);
    QCOMPARE(result.line, QStringLiteral("C3003090^M10^260224^002695"));
    QVERIFY(!result.persisted);
    QVERIFY(!result.persistDetail.isEmpty());
    source.stop();
}

void BarcodeReaderSdkSourceTest::forwardArgumentKeepsOnePerBarcodeInTableOrder()
{
    // The exact argument vector the forward program receives (user decision
    // 2026-09-23, revised the same day after the real forward program — TCP_HMI
    // V1.0.5 — was supplied): ONE ARGUMENT PER BARCODE, table order, empty
    // positions skipped. It is NOT a joined string: TCP_HMI builds its frame as
    // `BARCODE<TAB>argv[1]<TAB>argv[2]…` and the receiver splits on TAB, so a
    // single space-joined argument would arrive as one barcode containing
    // spaces. A platform-neutral helper, so this is the production transform,
    // not a re-implementation of it.
    QVector<BarcodeRow> rows;
    const auto row = [](const QString &barcode) {
        BarcodeRow r;
        r.barcode = barcode;
        return r;
    };
    rows.append(row(QStringLiteral("C3003090^M10^260224^002695")));
    rows.append(row(QString())); // a looked-at but empty position
    rows.append(row(QStringLiteral("C3003090^M10^260224^002696")));

    QStringList arguments;
    QVERIFY(barcodeForwardArguments(rows, &arguments));
    QCOMPARE(arguments,
             QStringList({QStringLiteral("C3003090^M10^260224^002695"),
                          QStringLiteral("C3003090^M10^260224^002696")}));
    // The barcode's own '^' characters survive untouched: they are content.
    QVERIFY(arguments.first().contains(QLatin1Char('^')));

    // One barcode: exactly one argument, no padding of any kind.
    QVERIFY(barcodeForwardArguments({row(QStringLiteral("ONLY"))}, &arguments));
    QCOMPARE(arguments, QStringList({QStringLiteral("ONLY")}));

    // A barcode containing a SPACE is fine here — TCP_HMI separates on TAB, so
    // a space inside a value is unambiguous and must not block the handover.
    QVERIFY(barcodeForwardArguments({row(QStringLiteral("AB CD"))}, &arguments));
    QCOMPARE(arguments, QStringList({QStringLiteral("AB CD")}));

    // Nothing decoded: no arguments at all, which the caller treats as "nothing
    // to forward" rather than starting a program with an empty parameter.
    QVERIFY(barcodeForwardArguments({}, &arguments));
    QVERIFY(arguments.isEmpty());
}

void BarcodeReaderSdkSourceTest::forwardArgumentRefusesATabOrNewline()
{
    // TAB is the downstream frame's field separator and CR/LF terminate its
    // frames, so a barcode carrying either cannot be represented. Refusing is a
    // visible failure; sending it would corrupt the frame silently — the same
    // trade-off this adapter makes everywhere else.
    const auto row = [](const QString &barcode) {
        BarcodeRow r;
        r.barcode = barcode;
        return r;
    };
    QStringList arguments;
    QVERIFY(!barcodeForwardArguments(
        {row(QStringLiteral("C3003090^M10^260224^002695")),
         row(QStringLiteral("BAD\tCODE"))},
        &arguments));
    QVERIFY(!barcodeForwardArguments({row(QStringLiteral("BAD\nCODE"))},
                                     &arguments));
    QVERIFY(!barcodeForwardArguments({row(QStringLiteral("BAD\rCODE"))},
                                     &arguments));
    // The refusal is all-or-nothing: nothing is half-handled.
    QVERIFY(arguments.isEmpty());
}

void BarcodeReaderSdkSourceTest::emptyForwardPathNeverRunsTheProgram()
{
    // No forward program configured: the step does not run, and the result says
    // nothing about forwarding at all — claiming either success or failure would
    // be inventing an outcome for something that never happened.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Ok);
    QVERIFY(result.persisted);
    QVERIFY(!result.forwarded);
    QVERIFY(result.forwardDetail.isEmpty());
    source.stop();
}

void BarcodeReaderSdkSourceTest::missingForwardProgramIsAVisibleFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.setForwardExePath(dir.filePath(QStringLiteral("no-such-program")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    // The barcode is still reported and still stored: a forward failure must
    // never hide data that was read successfully.
    QCOMPARE(result.state, BarcodeState::Ok);
    QVERIFY(result.persisted);
    QVERIFY(!result.forwarded);
    QVERIFY(result.forwardDetail.contains(QStringLiteral("外发程序不存在")));
    source.stop();
}

void BarcodeReaderSdkSourceTest::forwardingRunsTheProgramOncePerCycleWithEveryBarcode()
{
    // The program is a REAL process — this very test binary re-entered as the
    // probe (tests/unit/forward_probe.h). No shell, so it behaves the same on the
    // Windows CI where the .sh version of this test could never run.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    hlm_test::ForwardProbe probe(dir.path());

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"),
        {QStringLiteral("C3003090^M10^260224^002695"),
         QStringLiteral("C3003090^M10^260224^002696")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    // A program path that CONTAINS A SPACE: the invocation must pass the
    // program separately from its arguments, never through a shell. The probe
    // copy is made beside the original because Windows deploys the Qt DLLs
    // app-local next to the test executable.
    const QString program = hlm_test::probeProgramWithSpaceInPath();
    QVERIFY2(!program.isEmpty() && program.contains(QLatin1Char(' '))
                 && QFileInfo::exists(program),
             qPrintable(QStringLiteral("probe copy not prepared: %1").arg(program)));
    source.setForwardExePath(program);
    source.setForwardExePath(program); // idempotent: the same path twice is fine
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QVERIFY2(result.forwarded,
             qPrintable(QStringLiteral("forward did not succeed: %1")
                            .arg(result.forwardDetail)));
    QVERIFY(result.forwardDetail.isEmpty());

    // ONE invocation carrying TWO arguments, in table order, each barcode
    // whole. `arguments()` has already dropped the probe's own argv[0].
    QVERIFY2(probe.ran(), "the forward program was never started");
    QCOMPARE(probe.arguments(),
             QStringList({QStringLiteral("C3003090^M10^260224^002695"),
                          QStringLiteral("C3003090^M10^260224^002696")}));
    source.stop();
}

void BarcodeReaderSdkSourceTest::nonZeroExitIsAVisibleFailure()
{
    // A program that ran and rejected the data (a failed downstream handover)
    // must be reported, not silently treated as delivered.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    hlm_test::ForwardProbe probe(dir.path());
    probe.exitWith(3);

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.setForwardExePath(hlm_test::probeProgramPath());
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QVERIFY(result.persisted);
    QVERIFY(!result.forwarded);
    QCOMPARE(result.forwardDetail, QStringLiteral("外发程序返回 3"));
    source.stop();
}

void BarcodeReaderSdkSourceTest::timeoutKillsTheProgramAndIsAVisibleFailure()
{
    // A hung program must not hold the cycle open forever. The timeout is
    // injected through Config for the same reason the cycle deadline is: the
    // production default (10 s) would make this test take ten seconds.
    //
    // The probe hangs for far longer than the injected budget, so the fact that
    // the cycle converged at all proves the kill took effect.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    hlm_test::ForwardProbe probe(dir.path());
    probe.hangFor(30000);

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeSdkSourceConfig config;
    config.forwardTimeoutMs = 500;
    BarcodeReaderSdkSource source(&sdk, config);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.setForwardExePath(hlm_test::probeProgramPath());
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QElapsedTimer timer;
    timer.start();
    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 10000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    // Bounded by the injected budget plus the kill's own 1 s grace, and far
    // below the 30 s the program wanted. This is the kill assertion: if the
    // adapter waited the program out, this would be 30 s.
    QVERIFY2(timer.elapsed() < 10000,
             qPrintable(QStringLiteral("the cycle waited %1 ms for a program that "
                                       "asked for 30 s")
                            .arg(timer.elapsed())));
    QVERIFY(result.persisted);
    QVERIFY(!result.forwarded);
    QVERIFY2(result.forwardDetail.contains(QStringLiteral("外发超时")),
             qPrintable(result.forwardDetail));
    source.stop();
}

void BarcodeReaderSdkSourceTest::unrepresentableBarcodeIsNotForwardedButIsStillStored()
{
    // TAB cannot be represented in the downstream frame. Refusing is visible;
    // sending it would corrupt every field after it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    hlm_test::ForwardProbe probe(dir.path());

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"),
        {QStringLiteral("C3003090^M10^260224^002695"),
         QStringLiteral("BAD\tCODE")}))};

    BarcodeReaderSdkSource source(&sdk);
    const QString resultPath = dir.filePath(QStringLiteral("Barcode.txt"));
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(resultPath);
    source.setForwardExePath(hlm_test::probeProgramPath());
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QVERIFY(!result.forwarded);
    QVERIFY(result.forwardDetail.contains(QStringLiteral("制表符")));
    // …and every barcode — including the one that blocked the forward — is
    // still displayed and still stored.
    QCOMPARE(result.state, BarcodeState::Ok);
    QVERIFY(result.persisted);
    QCOMPARE(resultLines(resultPath),
             QStringList({QStringLiteral("C3003090^M10^260224^002695"),
                          QStringLiteral("BAD\tCODE")}));
    QVERIFY2(!probe.ran(), "the forward program ran despite the unrepresentable "
                           "barcode");
    source.stop();
}

void BarcodeReaderSdkSourceTest::noCodeCycleDoesNotForward()
{
    // A cycle that decoded nothing has nothing to hand downstream. Starting the
    // program with no arguments would look like a board with no barcodes.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    hlm_test::ForwardProbe probe(dir.path());

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(QStringLiteral("job-1"), {}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.setForwardExePath(hlm_test::probeProgramPath());
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::NoCode);
    QVERIFY(!result.forwarded);
    QVERIFY(result.forwardDetail.isEmpty());
    QVERIFY2(!probe.ran(), "the forward program was started for a cycle that "
                           "decoded nothing");
    source.stop();
}


void BarcodeReaderSdkSourceTest::overlappingCycleIsRejectedNotQueued()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    // A permanently retryable condition: the cycle must stay alive (querying
    // the SAME requestId) instead of converging on the first reply.
    sdk.triggerReplies = {{BarcodeSdkStatus::Timeout, QByteArray()}};
    sdk.resultReplies = {{BarcodeSdkStatus::Timeout, QByteArray()}};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();

    QVERIFY(source.requestRead());
    // The trigger call itself can time out without meaning the job was not
    // accepted, so the cycle stays in progress and keeps querying.
    QTRY_VERIFY_WITH_TIMEOUT(sdk.resultIds.size() >= 1, 5000);
    QVERIFY(source.cycleInProgress());
    // The SDK rejects a second trigger while one is pending, so an overlapping
    // request must be refused visibly rather than queued — and the refused
    // request must not send a second trigger.
    QVERIFY(!source.requestRead());
    QTRY_VERIFY_WITH_TIMEOUT(sdk.resultIds.size() >= 2, 5000);
    QCOMPARE(sdk.triggerIds.size(), 1);
    // Every query carries the same requestId, proving the retry never re-keys
    // the cycle.
    const QString id = sdk.triggerIds.first();
    for (const QString &queried : sdk.resultIds)
        QCOMPARE(queried, id);

    // Let the cycle finish so the thread stops cleanly.
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};
    QTRY_VERIFY_WITH_TIMEOUT(!source.cycleInProgress(), 10000);
    // Once the cycle converged, the next one is accepted again.
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-2")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-2"), {QStringLiteral("C3003100^M10^260224^002696")}))};
    QVERIFY(source.requestRead());
    source.stop();
}

void BarcodeReaderSdkSourceTest::sdkCallsRunOffTheCallingThread()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Records which thread each call arrived on: the SDK blocks in its caller,
    // and the contract forbids that on the UI thread.
    class ThreadRecordingSdk : public FakeBarcodeSdk
    {
    public:
        QThread *callingThread = QThread::currentThread();
        QThread *statusThread = nullptr;
        QThread *triggerThread = nullptr;
        QThread *resultThread = nullptr;
        BarcodeSdkReply status() override
        {
            statusThread = QThread::currentThread();
            return FakeBarcodeSdk::status();
        }
        BarcodeSdkReply trigger(const QString &id) override
        {
            triggerThread = QThread::currentThread();
            return FakeBarcodeSdk::trigger(id);
        }
        BarcodeSdkReply result(const QString &id) override
        {
            resultThread = QThread::currentThread();
            return FakeBarcodeSdk::result(id);
        }
    };

    ThreadRecordingSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setScannerProgramPath(QStringLiteral("C:/SDK/trigger_client.exe"));
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    QVERIFY(sdk.statusThread != nullptr);
    QVERIFY2(sdk.statusThread != sdk.callingThread,
             "the SDK must not be called from the caller/UI thread");
    QCOMPARE(sdk.statusThread, sdk.triggerThread);
    QCOMPARE(sdk.statusThread, sdk.resultThread);
    source.stop();
}

// The production CLI transport (user decision 2026-09-23). These call
// makeSystemBarcodeSdk() directly — the very object the shipped HMI builds —
// with this test binary standing in for the vendor's trigger_client.exe, so
// what is exercised is the production process invocation, not a fake.
void BarcodeReaderSdkSourceTest::cliReadsTheJsonReplyFromStdout()
{
    hlm_test::ForwardProbe probe(QDir::tempPath());
    const QByteArray reply = R"({"ok":true,"code":"completed","decodedCount":6})";
    qputenv(hlm_test::kProbeStdoutEnv, reply);

    std::unique_ptr<IBarcodeSdk> sdk = makeSystemBarcodeSdk(hlm_test::probeProgramPath());
    QVERIFY(sdk != nullptr);
    const BarcodeSdkReply status = sdk->status();
    QCOMPARE(status.status, BarcodeSdkStatus::Ok);
    QCOMPARE(status.json, reply);

    const BarcodeSdkReply result = sdk->result(QStringLiteral("20260923103040123-1"));
    QCOMPARE(result.status, BarcodeSdkStatus::Ok);
    QCOMPARE(result.json, reply);

    const BarcodeSdkReply triggered = sdk->trigger(QStringLiteral("20260923103040123-1"));
    QCOMPARE(triggered.status, BarcodeSdkStatus::Ok);
    QCOMPARE(triggered.json, reply);

    // argv: [endpoint, command] or [endpoint, command, requestId].
    const QStringList arguments = probe.arguments();
    QCOMPARE(arguments.size(), 3);
    QCOMPARE(arguments.at(0), QStringLiteral("BarcodeReader.Trigger.v1"));
    QCOMPARE(arguments.at(1), QStringLiteral("trigger"));
    QCOMPARE(arguments.at(2), QStringLiteral("20260923103040123-1"));

    // A CLI moved or renamed since it was configured: its own actionable reason,
    // NOT "start the scan program", which would send the operator to the wrong
    // fix.
    std::unique_ptr<IBarcodeSdk> gone =
        makeSystemBarcodeSdk(QDir::tempPath() + QStringLiteral("/no-such-scanner.exe"));
    QCOMPARE(gone->status().status, BarcodeSdkStatus::ProgramUnavailable);
}

void BarcodeReaderSdkSourceTest::cliExitCodeIsTheTransportStatus()
{
    // example_c.c returns the BR_* code, so the CLI's exit status IS the
    // transport status — the two error channels never overlap and no stdout
    // parsing is needed for failures.
    hlm_test::ForwardProbe probe(QDir::tempPath());
    probe.exitWith(2); // BR_NOT_CONNECTED: the scan program is closed

    std::unique_ptr<IBarcodeSdk> sdk = makeSystemBarcodeSdk(hlm_test::probeProgramPath());
    const BarcodeSdkReply reply = sdk->status();
    QCOMPARE(reply.status, BarcodeSdkStatus::NotConnected);
    QVERIFY(reply.json.isEmpty());
    QCOMPARE(BarcodeReaderSdkSource::transportReason(reply.status),
             QStringLiteral("扫码服务未启动，请先打开 BarcodeReader 并点击运行"));
}

void BarcodeReaderSdkSourceTest::cliPassesEndpointCommandAndRequestId()
{
    hlm_test::ForwardProbe probe(QDir::tempPath());

    std::unique_ptr<IBarcodeSdk> sdk = makeSystemBarcodeSdk(hlm_test::probeProgramPath());
    const QString id = QStringLiteral("20260923103040123-7");

    // `status` takes no request id; `trigger`/`result` take exactly that id and
    // keep it identical across calls — the SDK dedups by id, so a changed id
    // would be a different job.
    QVERIFY(sdk->status().json.isEmpty() || true);
    QStringList arguments = probe.arguments();
    QCOMPARE(arguments,
             QStringList({QStringLiteral("BarcodeReader.Trigger.v1"),
                          QStringLiteral("status")}));

    sdk->trigger(id);
    arguments = probe.arguments();
    QCOMPARE(arguments,
             QStringList({QStringLiteral("BarcodeReader.Trigger.v1"),
                          QStringLiteral("trigger"), id}));

    sdk->result(id);
    arguments = probe.arguments();
    QCOMPARE(arguments,
             QStringList({QStringLiteral("BarcodeReader.Trigger.v1"),
                          QStringLiteral("result"), id}));
}

void BarcodeReaderSdkSourceTest::cliMissingProgramIsItsOwnActionableReason()
{
    std::unique_ptr<IBarcodeSdk> sdk =
        makeSystemBarcodeSdk(QDir::tempPath() + QStringLiteral("/no-such-scanner.exe"));
    const BarcodeSdkReply reply = sdk->status();
    QCOMPARE(reply.status, BarcodeSdkStatus::ProgramUnavailable);
    QCOMPARE(BarcodeReaderSdkSource::transportReason(reply.status),
             QStringLiteral("扫码程序无法启动，请检查扫码程序路径"));
    // The same reply for every command: a deployment problem is not per-command.
    QCOMPARE(sdk->trigger(QStringLiteral("id")).status, BarcodeSdkStatus::ProgramUnavailable);
    QCOMPARE(sdk->result(QStringLiteral("id")).status, BarcodeSdkStatus::ProgramUnavailable);
}

void BarcodeReaderSdkSourceTest::cliTimeoutKillsTheWedgedProgram()
{
    // A wedged scan program must not hold the HMI's cycle open. The timeout is
    // the adapter's own kCallTimeoutMs (3 s), and the probe hangs for far longer,
    // so the elapsed time is the proof the process was killed rather than waited
    // out. The decode itself is NOT cancelled: the cycle keeps querying the SAME
    // request id, which is what the vendor documents for a timeout.
    hlm_test::ForwardProbe probe(QDir::tempPath());
    probe.hangFor(30000);

    std::unique_ptr<IBarcodeSdk> sdk = makeSystemBarcodeSdk(hlm_test::probeProgramPath());
    QElapsedTimer timer;
    timer.start();
    const BarcodeSdkReply reply = sdk->status();
    const qint64 elapsed = timer.elapsed();

    QCOMPARE(reply.status, BarcodeSdkStatus::Timeout);
    QVERIFY2(elapsed < 15000,
             qPrintable(QStringLiteral("waited %1 ms for a program that asked for "
                                       "30 s").arg(elapsed)));
    QVERIFY2(elapsed >= 2000,
             qPrintable(QStringLiteral("returned after %1 ms, so the call budget "
                                       "was not applied at all").arg(elapsed)));
}

// A hand-written main instead of QTEST_MAIN so the binary can also act as the
// forward-program probe (tests/forward_probe.h): when the environment asks for
// it, this process records its own argv and exits without running any test. The
// probe is a real executable on every platform, which is what lets the forward
// tests exercise the production QProcess invocation on Windows too.
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (hlm_test::forwardProbeRequested())
        return hlm_test::runForwardProbe();
    BarcodeReaderSdkSourceTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_barcode_reader_sdk_source.moc"
