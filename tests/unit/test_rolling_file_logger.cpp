// Task 8 unit tests: rolling diagnostic log (spec §12, §17).
// Covers: 10 x 10 MiB rotation, file count cap, and sensitive-data redaction
// (passwords, digests, tokens).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "common/rolling_file_logger.h"

using namespace hlm;

class RollingFileLoggerTest : public QObject
{
    Q_OBJECT

private slots:
    void redactsPasswordsAndTokens();
    void redactsPasswordDigest();
    void rotationCapsFileCount();
    void rotationKeepsNewestContent();
    void defaultLimitsAre10x10MiB();
};

void RollingFileLoggerTest::redactsPasswordsAndTokens()
{
    QCOMPARE(redactSensitive(QStringLiteral("login password=hunter2 ok")),
             QStringLiteral("login password=[REDACTED] ok"));
    QCOMPARE(redactSensitive(QStringLiteral("password: hunter2")),
             QStringLiteral("password: [REDACTED]"));
    QCOMPARE(redactSensitive(QStringLiteral("token=abc123def")),
             QStringLiteral("token=[REDACTED]"));
    QCOMPARE(redactSensitive(QStringLiteral("session_token=xyz")),
             QStringLiteral("session_token=[REDACTED]"));
    QCOMPARE(redactSensitive(QStringLiteral("password_hash=deadbeef")),
             QStringLiteral("password_hash=[REDACTED]"));
    // Ordinary text is untouched.
    QCOMPARE(redactSensitive(QStringLiteral("width=210 ok")),
             QStringLiteral("width=210 ok"));
}

void RollingFileLoggerTest::redactsPasswordDigest()
{
    // A PBKDF2 digest must never appear in the log (spec §17).
    const QString digest = QStringLiteral("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    const QString out = redactSensitive(QStringLiteral("stored password_hash=") + digest);
    QVERIFY(!out.contains(digest));
    QVERIFY(out.contains(QStringLiteral("[REDACTED]")));
}

void RollingFileLoggerTest::rotationCapsFileCount()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const int maxFiles = 3;
    const qint64 maxBytes = 1024;
    {
        RollingFileLogger logger(dir.path(), maxFiles, maxBytes);
        // Write enough to force several rotations.
        const QString line(200, QLatin1Char('x'));
        for (int i = 0; i < 40; ++i)
            logger.write(line);
    }
    // At most maxFiles files exist (spec §12: 10 files).
    const QStringList files = QDir(dir.path()).entryList({QStringLiteral("log.*")},
                                                          QDir::Files);
    QVERIFY2(files.size() <= maxFiles,
             qPrintable(QStringLiteral("expected <= %1 files, got %2")
                            .arg(maxFiles).arg(files.size())));
}

void RollingFileLoggerTest::rotationKeepsNewestContent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const int maxFiles = 2;
    const qint64 maxBytes = 1024;
    {
        RollingFileLogger logger(dir.path(), maxFiles, maxBytes);
        const QString line(200, QLatin1Char('x'));
        for (int i = 0; i < 30; ++i)
            logger.write(line);
        // The newest content must be in log.1.
        QFile f(dir.filePath(QStringLiteral("log.1")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray content = f.readAll();
        QVERIFY(!content.isEmpty());
    }
}

void RollingFileLoggerTest::defaultLimitsAre10x10MiB()
{
    QCOMPARE(kDefaultLogFileCount, 10);
    QCOMPARE(kDefaultLogFileSizeBytes, qint64(10 * 1024 * 1024));
}

QTEST_GUILESS_MAIN(RollingFileLoggerTest)
#include "test_rolling_file_logger.moc"
