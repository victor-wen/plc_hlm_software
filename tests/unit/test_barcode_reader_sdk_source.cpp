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

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include "adapters/barcode/barcode_reader_sdk_source.h"

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
    void completedCycleParsesEveryRowAndAppendsEachBarcode();
    void emptyPositionsAreCountedAndNeverWritten();
    void completedCycleWithoutAnyBarcodeIsNoCode();
    void absentSdkConvergesToTheOperatorReason();
    void rejectedTriggerCarriesTheBusinessReason();
    void serverRestartNeverTrustsTheResult();
    void truncatedResultIsAFailureNotNoCode();
    void transportTimeoutKeepsPollingTheSameRequestId();
    void cycleDeadlineConvergesTheWaitNotTheDecode();
    void busyTriggerIsTerminalBecauseNothingWasAccepted();
    void busyKeepsPollingTheSameRequestId();
    void twoCyclesUseDifferentRequestIds();
    void failedWriteStillShowsTheBarcode();
    void forwardArgumentJoinsBarcodesInTableOrder();
    void forwardArgumentRefusesABarcodeContainingASpace();
    void emptyForwardPathNeverRunsTheProgram();
    void missingForwardProgramIsAVisibleFailure();
    void forwardingRunsTheProgramOncePerCycleWithEveryBarcode();
    void nonZeroExitIsAVisibleFailure();
    void timeoutKillsTheProgramAndIsAVisibleFailure();
    void barcodeContainingASpaceIsNotForwardedButIsStillStored();
    void noCodeCycleDoesNotForward();
    void overlappingCycleIsRejectedNotQueued();
    void sdkCallsRunOffTheCallingThread();
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
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    BarcodeReaderSdkSource source(&sdk);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::NotConfigured);
    // A missing path must not reach the SDK at all.
    QCOMPARE(sdk.statusCalls, 0);
    QCOMPARE(sdk.triggerIds.size(), 0);
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
    sdk.statusReplies = {{BarcodeSdkStatus::LibraryUnavailable, QByteArray()}};

    BarcodeReaderSdkSource source(&sdk);
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

void BarcodeReaderSdkSourceTest::rejectedTriggerCarriesTheBusinessReason()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(
        R"({"ok":false,"code":"wrong_mode","state":"failed","serverId":"server-1"})")};

    BarcodeReaderSdkSource source(&sdk);
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

void BarcodeReaderSdkSourceTest::forwardArgumentJoinsBarcodesInTableOrder()
{
    // The exact join the forward program receives (user decision 2026-09-23):
    // table order, ONE space between barcodes, empty positions skipped, nothing
    // added around the whole string. A platform-neutral helper, so this is the
    // production code path, not a re-implementation.
    QVector<BarcodeRow> rows;
    const auto addRow = [&rows](const QString &barcode) {
        BarcodeRow row;
        row.barcode = barcode;
        rows.append(row);
    };
    addRow(QStringLiteral("C3003090^M10^260224^002695"));
    addRow(QString());                       // a looked-at but empty position
    addRow(QStringLiteral("C3003090^M10^260224^002696"));

    QString argument;
    QVERIFY(barcodeForwardArgument(rows, &argument));
    QCOMPARE(argument,
             QStringLiteral("C3003090^M10^260224^002695 "
                            "C3003090^M10^260224^002696"));

    // One barcode: no separator, no padding.
    QVector<BarcodeRow> single;
    addRow(QStringLiteral("ONLY"));
    BarcodeRow only;
    only.barcode = QStringLiteral("ONLY");
    single.append(only);
    QVERIFY(barcodeForwardArgument(single, &argument));
    QCOMPARE(argument, QStringLiteral("ONLY"));

    // Nothing decoded: an empty argument, which the caller treats as "nothing
    // to forward" rather than starting a program with an empty parameter.
    QVERIFY(barcodeForwardArgument({}, &argument));
    QVERIFY(argument.isEmpty());
}

void BarcodeReaderSdkSourceTest::forwardArgumentRefusesABarcodeContainingASpace()
{
    // One space is the separator, so a barcode that itself contains one cannot
    // be told apart from two barcodes by the receiving program. Refusing is a
    // visible failure; sending it would corrupt the data silently.
    QVector<BarcodeRow> rows;
    BarcodeRow clean;
    clean.barcode = QStringLiteral("C3003090^M10^260224^002695");
    BarcodeRow dirty;
    dirty.barcode = QStringLiteral("BAD CODE");
    rows.append(clean);
    rows.append(dirty);

    QString argument;
    QVERIFY(!barcodeForwardArgument(rows, &argument));
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
    // A real program, on whichever platform this runs: a shell script that
    // records its argv verbatim. QProcess is a real process on every platform,
    // so this exercises the production invocation, path and all.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("argv.txt"));
    const QString program = dir.filePath(QStringLiteral("record-argv.sh"));
    {
        QFile script(program);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\n");
        // One line per argument, prefixed with the count on the first line, so
        // the test can prove BOTH the argument count and the exact text.
        script.write("echo \"argc=$#\" > \"$OUT\"\n");
        script.write("for a in \"$@\"; do echo \"$a\" >> \"$OUT\"; done\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
    }
    qputenv("OUT", output.toUtf8());

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"),
        {QStringLiteral("C3003090^M10^260224^002695"),
         QStringLiteral("C3003090^M10^260224^002696")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
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

    // Exactly ONE argument, carrying both barcodes space-joined in table order.
    QFile recorded(output);
    QVERIFY(recorded.open(QIODevice::ReadOnly));
    const QStringList lines =
        QString::fromUtf8(recorded.readAll())
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    recorded.close();
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines[0], QStringLiteral("argc=1"));
    QCOMPARE(lines[1], QStringLiteral("C3003090^M10^260224^002695 "
                                      "C3003090^M10^260224^002696"));
    source.stop();
}

// A cycle that decoded nothing has nothing to hand downstream. Starting the
// program with an empty argument would look like a board with no barcodes.
void BarcodeReaderSdkSourceTest::nonZeroExitIsAVisibleFailure()
{
    // A program that ran and rejected the data (a failed downstream handover)
    // must be reported, not silently treated as delivered.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString program = dir.filePath(QStringLiteral("fail.sh"));
    {
        QFile script(program);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nexit 3\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
    }

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.setForwardExePath(program);
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString program = dir.filePath(QStringLiteral("hang.sh"));
    {
        QFile script(program);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nsleep 30\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
    }

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"), {QStringLiteral("C3003090^M10^260224^002695")}))};

    BarcodeSdkSourceConfig config;
    config.forwardTimeoutMs = 500;
    BarcodeReaderSdkSource source(&sdk, config);
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.setForwardExePath(program);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 10000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QVERIFY(result.persisted);
    QVERIFY(!result.forwarded);
    // 500 ms / 1000 is 0 whole seconds, so the message reads "外发超时（0 秒）";
    // the important part is that it says 超时 and the cycle converged.
    QVERIFY2(result.forwardDetail.contains(QStringLiteral("外发超时")),
             qPrintable(result.forwardDetail));
    source.stop();
}

void BarcodeReaderSdkSourceTest::barcodeContainingASpaceIsNotForwardedButIsStillStored()
{
    // The join cannot represent a barcode that contains the separator. Refusing
    // is visible; sending it would hand the downstream program a value it would
    // silently split in the wrong place.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("argv.txt"));
    const QString program = dir.filePath(QStringLiteral("record-argv.sh"));
    {
        QFile script(program);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\necho \"argc=$#\" > \"$OUT\"\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
    }
    qputenv("OUT", output.toUtf8());

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(
        QStringLiteral("job-1"),
        {QStringLiteral("C3003090^M10^260224^002695"), QStringLiteral("BAD CODE")}))};

    BarcodeReaderSdkSource source(&sdk);
    const QString resultPath = dir.filePath(QStringLiteral("Barcode.txt"));
    source.setResultPath(resultPath);
    source.setForwardExePath(program);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QVERIFY(!result.forwarded);
    QVERIFY(result.forwardDetail.contains(QStringLiteral("空格")));
    // …and both barcodes — including the one that blocked the forward — are
    // still displayed and still stored.
    QCOMPARE(result.state, BarcodeState::Ok);
    QVERIFY(result.persisted);
    QCOMPARE(resultLines(resultPath),
             QStringList({QStringLiteral("C3003090^M10^260224^002695"),
                          QStringLiteral("BAD CODE")}));
    QVERIFY2(!QFile::exists(output), "the forward program ran despite the "
                                     "ambiguous join");
    source.stop();
}

void BarcodeReaderSdkSourceTest::noCodeCycleDoesNotForward()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString output = dir.filePath(QStringLiteral("argv.txt"));
    const QString program = dir.filePath(QStringLiteral("record-argv.sh"));
    {
        QFile script(program);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\necho \"argc=$#\" > \"$OUT\"\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner
                                      | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner));
    }
    qputenv("OUT", output.toUtf8());

    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    sdk.resultReplies = {okReply(completedJson(QStringLiteral("job-1"), {}))};

    BarcodeReaderSdkSource source(&sdk);
    source.setResultPath(dir.filePath(QStringLiteral("Barcode.txt")));
    source.setForwardExePath(program);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    QVERIFY(source.requestRead());
    QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 5000);
    const BarcodeResult result = spy[0][0].value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::NoCode);
    QVERIFY(!result.forwarded);
    QVERIFY(result.forwardDetail.isEmpty());
    QVERIFY2(!QFile::exists(output), "the forward program was started for a "
                                     "cycle that decoded nothing");
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

QTEST_MAIN(BarcodeReaderSdkSourceTest)
#include "test_barcode_reader_sdk_source.moc"
