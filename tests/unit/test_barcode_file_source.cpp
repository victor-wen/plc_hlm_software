// Unit tests: file-based barcode result source (user decision 2026-09-22).
//
// The scanning program is external; this adapter only reads the text file it
// writes. Coverage:
// - NotConfigured: an empty path never produces a value (the UI must keep
//   showing 未配置 rather than pretending the feature works).
// - Ok: the LAST non-empty line wins (the reference file is an append-only
//   history), CRLF is stripped, and the line is split on '^'.
// - NoNewResult: an unchanged file is NOT this cycle's barcode — the scanning
//   program leaves the previous text in place when it decodes nothing.
// - Failed: missing, unreadable, empty and oversized files each converge to a
//   visible reason instead of silence.
// - Changing the path drops the baseline, so the first read of a new file is
//   never judged against the previous file's timestamp.
// - All I/O runs on the adapter's own worker thread: requestRead() returns
//   immediately and the result arrives asynchronously.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "adapters/barcode/barcode_file_source.h"

using namespace hlm;

namespace {

// The reference format (需求/扫码相关/Barcode.txt): CRLF-terminated lines,
// '^'-separated fields. '^M' is literal text, not a control character.
const char *kSample =
    "C3003090^M10^260224^002700\r\n"
    "C3003100^M10^260224^002695\r\n"
    "C3003090^M10^260224^002699\r\n";

void writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
             qPrintable(file.errorString()));
    file.write(data);
    file.close();
}

// Appends `data`. The adapter's staleness baseline is the (mtime, size) pair,
// and an append always changes the size, so no timestamp manipulation is
// needed to make the change observable.
void appendFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::Append), qPrintable(file.errorString()));
    file.write(data);
    file.close();
}

} // namespace

class BarcodeFileSourceTest : public QObject
{
    Q_OBJECT

private slots:
    void notConfiguredWithoutAPath();
    void readsTheLastLineAndSplitsFields();
    void unchangedFileIsNotThisCyclesResult();
    void missingFileConvergesToAFailure();
    void emptyFileConvergesToAFailure();
    void oversizedFileConvergesToAFailure();
    void changingThePathDropsTheBaseline();
};

void BarcodeFileSourceTest::notConfiguredWithoutAPath()
{
    BarcodeFileSource source;
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    const auto result = spy.at(0).at(0).value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::NotConfigured);
    QVERIFY(result.line.isEmpty());
    QVERIFY(result.fields.isEmpty());
    source.stop();
}

void BarcodeFileSourceTest::readsTheLastLineAndSplitsFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));
    writeFile(path, kSample);

    BarcodeFileSource source;
    source.setResultPath(path);
    QCOMPARE(source.resultPath(), path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    const auto result = spy.at(0).at(0).value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Ok);
    // Last line wins; the CRLF must not leak into the value.
    QCOMPARE(result.line, QStringLiteral("C3003090^M10^260224^002699"));
    QCOMPARE(result.fields.size(), 4);
    QCOMPARE(result.fields.at(0), QStringLiteral("C3003090"));
    QCOMPARE(result.fields.at(1), QStringLiteral("M10"));
    QCOMPARE(result.fields.at(2), QStringLiteral("260224"));
    QCOMPARE(result.fields.at(3), QStringLiteral("002699"));
    QVERIFY(result.sequence > 0);
    source.stop();
}

void BarcodeFileSourceTest::unchangedFileIsNotThisCyclesResult()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));
    writeFile(path, kSample);

    BarcodeFileSource source;
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QCOMPARE(spy.at(0).at(0).value<BarcodeResult>().state, BarcodeState::Ok);

    // The scanning program decoded nothing and left the previous text in
    // place: the same content must never be reported as this cycle's barcode.
    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 5000);
    const auto second = spy.at(1).at(0).value<BarcodeResult>();
    QCOMPARE(second.state, BarcodeState::NoNewResult);
    QVERIFY(!second.detail.isEmpty());
    QVERIFY(second.line.isEmpty());

    // A new line makes the next read a real result again.
    appendFile(path, "C3003100^M10^260224^002701\r\n");
    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 3, 5000);
    const auto third = spy.at(2).at(0).value<BarcodeResult>();
    QCOMPARE(third.state, BarcodeState::Ok);
    QCOMPARE(third.line, QStringLiteral("C3003100^M10^260224^002701"));
    source.stop();
}

void BarcodeFileSourceTest::missingFileConvergesToAFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    BarcodeFileSource source;
    source.setResultPath(dir.filePath(QStringLiteral("absent.txt")));
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    const auto result = spy.at(0).at(0).value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(!result.detail.isEmpty());
    source.stop();
}

void BarcodeFileSourceTest::emptyFileConvergesToAFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));
    writeFile(path, QByteArray());

    BarcodeFileSource source;
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    const auto result = spy.at(0).at(0).value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(!result.detail.isEmpty());
    source.stop();
}

void BarcodeFileSourceTest::oversizedFileConvergesToAFailure()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("Barcode.txt"));
    // Larger than the 1 MiB cap: a misconfigured path must not stall the
    // worker thread reading it.
    QByteArray big(1024 * 1024 + 16, 'A');
    big.append('\n');
    writeFile(path, big);

    BarcodeFileSource source;
    source.setResultPath(path);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    const auto result = spy.at(0).at(0).value<BarcodeResult>();
    QCOMPARE(result.state, BarcodeState::Failed);
    QVERIFY(!result.detail.isEmpty());
    source.stop();
}

void BarcodeFileSourceTest::changingThePathDropsTheBaseline()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a.txt"));
    const QString second = dir.filePath(QStringLiteral("b.txt"));
    writeFile(first, "AAA^1\r\n");
    writeFile(second, "BBB^2\r\n");

    BarcodeFileSource source;
    source.setResultPath(first);
    source.start();
    QSignalSpy spy(&source, &IBarcodeSource::resultReady);

    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
    QCOMPARE(spy.at(0).at(0).value<BarcodeResult>().line, QStringLiteral("AAA^1"));

    // Switching files must not judge the new file against the old baseline,
    // even though the (mtime, size) pair could coincide.
    source.setResultPath(second);
    source.requestRead();
    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 2, 5000);
    const auto secondResult = spy.at(1).at(0).value<BarcodeResult>();
    QCOMPARE(secondResult.state, BarcodeState::Ok);
    QCOMPARE(secondResult.line, QStringLiteral("BBB^2"));
    source.stop();
}

QTEST_GUILESS_MAIN(BarcodeFileSourceTest)
#include "test_barcode_file_source.moc"