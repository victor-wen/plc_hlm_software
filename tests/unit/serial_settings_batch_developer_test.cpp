// PLC-HMI-004 developer regression test (D7): the serial-settings batch is
// atomic even when a write fails part-way through the seven keys.
//
// The independent integration test injects a whole-transaction commit failure
// (SQLite write lock held by the test). This developer test injects a failure
// exactly mid-batch with a temporary app_settings trigger, which needs no
// production test seam, and asserts that every previously committed key is
// unchanged and no replacement value is observable.

#include <QtTest>

#include <QPair>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVector>

#include "adapters/sqlite/sqlite_repositories.h"
#include "domain/serial_connection_settings.h"
#include "sqlite_test_util.h"

using namespace hlm;

namespace {

constexpr const char *kPortKey = "serial.comPort";
constexpr const char *kStationKey = "serial.station";
constexpr const char *kBaudKey = "serial.baudRate";
constexpr const char *kStopKey = "serial.stopBits";
constexpr const char *kParityKey = "serial.parity";
constexpr const char *kTimeoutKey = "serial.timeoutMs";
constexpr const char *kRetriesKey = "serial.readRetries";

QVector<QPair<QString, QString>> baselineRows()
{
    return {
        {QString::fromLatin1(kPortKey), QStringLiteral("COM1")},
        {QString::fromLatin1(kStationKey), QStringLiteral("1")},
        {QString::fromLatin1(kBaudKey), QStringLiteral("9600")},
        {QString::fromLatin1(kStopKey), QStringLiteral("1")},
        {QString::fromLatin1(kParityKey), QStringLiteral("无")},
        {QString::fromLatin1(kTimeoutKey), QStringLiteral("200")},
        {QString::fromLatin1(kRetriesKey), QStringLiteral("1")},
    };
}

SerialConnectionSettings replacementSettings()
{
    SerialConnectionSettings settings;
    settings.port_name = QStringLiteral("COM7");
    settings.station = 2;
    settings.baud_rate = 19200;
    settings.stop_bits = 2;
    settings.parity = QStringLiteral("偶");
    settings.timeout_ms = 500;
    settings.read_retries = 3;
    return settings;
}

bool seedBaseline(SqliteSettingsRepository &repo, QString *error)
{
    for (const auto &row : baselineRows()) {
        SettingRecord record;
        record.key = row.first;
        record.typedValue = row.second;
        record.updatedBy = QStringLiteral("admin");
        if (!repo.setSetting(record, error))
            return false;
    }
    return true;
}

QString storedValue(SqliteSettingsRepository &repo, const QString &key)
{
    const std::optional<SettingRecord> record = repo.getSetting(key);
    return record ? record->typedValue : QString();
}

} // namespace

class SerialSettingsBatchDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void committedBatchReplacesAllSevenKeys();
    void midBatchFailureLeavesEveryKeyUnchanged();
};

void SerialSettingsBatchDeveloperTest::initTestCase()
{
    QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
             "QSQLITE driver not available");
}

void SerialSettingsBatchDeveloperTest::cleanup()
{
    QSqlDatabase::removeDatabase(QStringLiteral("serial_batch_dev"));
}

void SerialSettingsBatchDeveloperTest::committedBatchReplacesAllSevenKeys()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db =
            hlm_test::createMigratedDb(dir, QStringLiteral("serial_batch_dev"));
        QVERIFY(db.isOpen());
        SqliteSettingsRepository repo(db);
        QString error;
        QVERIFY2(seedBaseline(repo, &error), qPrintable(error));

        SettingsBatch batch;
        batch.batch_id = 11;
        batch.settings = replacementSettings();
        batch.updated_by = QStringLiteral("admin");
        QVERIFY2(repo.saveSerialSettingsBatch(batch, &error), qPrintable(error));

        QCOMPARE(storedValue(repo, QString::fromLatin1(kPortKey)),
                 QStringLiteral("COM7"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kStationKey)),
                 QStringLiteral("2"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kBaudKey)),
                 QStringLiteral("19200"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kStopKey)),
                 QStringLiteral("2"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kParityKey)),
                 QStringLiteral("偶"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kTimeoutKey)),
                 QStringLiteral("500"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kRetriesKey)),
                 QStringLiteral("3"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("serial_batch_dev"));
}

void SerialSettingsBatchDeveloperTest::midBatchFailureLeavesEveryKeyUnchanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db =
            hlm_test::createMigratedDb(dir, QStringLiteral("serial_batch_dev"));
        QVERIFY(db.isOpen());
        SqliteSettingsRepository repo(db);
        QString error;
        QVERIFY2(seedBaseline(repo, &error), qPrintable(error));

        // Injected failure exactly on the third of the seven keys: the first
        // two inserts of the transaction succeed and the third aborts, so the
        // rollback has to restore them.
        QSqlQuery trigger(db);
        QVERIFY2(trigger.exec(QStringLiteral(
                     "CREATE TRIGGER plc_hmi_004_mid_batch_failure"
                     " BEFORE INSERT ON app_settings"
                     " WHEN NEW.key = 'serial.baudRate'"
                     " BEGIN SELECT RAISE(ABORT, 'injected mid-batch failure'); END")),
                 qPrintable(trigger.lastError().text()));

        SettingsBatch batch;
        batch.batch_id = 12;
        batch.settings = replacementSettings();
        batch.updated_by = QStringLiteral("admin");
        error.clear();
        QVERIFY2(!repo.saveSerialSettingsBatch(batch, &error),
                 "the injected mid-batch failure must fail the batch");
        QVERIFY2(!error.isEmpty(), "a failed batch must carry an error");

        // Every previously committed value survives and no replacement value
        // is observable anywhere (all-or-none).
        QCOMPARE(storedValue(repo, QString::fromLatin1(kPortKey)),
                 QStringLiteral("COM1"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kStationKey)),
                 QStringLiteral("1"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kBaudKey)),
                 QStringLiteral("9600"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kStopKey)),
                 QStringLiteral("1"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kParityKey)),
                 QStringLiteral("无"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kTimeoutKey)),
                 QStringLiteral("200"));
        QCOMPARE(storedValue(repo, QString::fromLatin1(kRetriesKey)),
                 QStringLiteral("1"));

        QSqlQuery scan(db);
        QVERIFY(scan.exec(QStringLiteral("SELECT typed_value FROM app_settings")));
        while (scan.next()) {
            const QString value = scan.value(0).toString();
            QVERIFY2(value != QStringLiteral("COM7"), "no replacement key was written");
            QVERIFY2(value != QStringLiteral("19200"), "no replacement key was written");
            QVERIFY2(value != QStringLiteral("500"), "no replacement key was written");
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("serial_batch_dev"));
}

QTEST_GUILESS_MAIN(SerialSettingsBatchDeveloperTest)
#include "serial_settings_batch_developer_test.moc"
