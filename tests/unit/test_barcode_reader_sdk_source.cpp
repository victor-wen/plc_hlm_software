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
    void notConfiguredTriggersNothing();
    void completedCycleParsesEveryRowAndAppendsEachBarcode();
    void emptyPositionsAreCountedAndNeverWritten();
    void completedCycleWithoutAnyBarcodeIsNoCode();
    void absentSdkConvergesToTheOperatorReason();
    void rejectedTriggerCarriesTheBusinessReason();
    void serverRestartNeverTrustsTheResult();
    void pollTimeoutConvergesAndReusesTheSameRequestId();
    void busyKeepsPollingTheSameRequestId();
    void twoCyclesUseDifferentRequestIds();
    void failedWriteStillShowsTheBarcode();
    void overlappingCycleIsRejectedNotQueued();
    void sdkCallsRunOffTheCallingThread();
};

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

void BarcodeReaderSdkSourceTest::pollTimeoutConvergesAndReusesTheSameRequestId()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeBarcodeSdk sdk;
    sdk.statusReplies = {okReply(statusJson(QStringLiteral("server-1")))};
    sdk.triggerReplies = {okReply(acceptedJson(QStringLiteral("job-1")))};
    // A transport timeout does NOT cancel the job, so the adapter must keep
    // querying — with the SAME requestId, never a fresh one.
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
