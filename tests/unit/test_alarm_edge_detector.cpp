// Task 8 unit tests: alarm edge detection (spec §12).
// Covers: 0 -> non-zero start, code change ends old + starts new, D110=0 with
// M14/M4 clear ends, M14/M4 set keeps the event active, unknown codes are
// preserved with a generic message, and HMI alarm start/end.

#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QTemporaryDir>

#include "adapters/sqlite/alarm_edge_detector.h"
#include "adapters/sqlite/sqlite_repositories.h"
#include "sqlite_test_util.h"

using namespace hlm;

class AlarmEdgeDetectorTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void zeroToNonZeroStartsEvent();
    void codeChangeEndsOldAndStartsNew();
    void zeroWithM14M4ClearEndsEvent();
    void zeroWithM14OrM4SetKeepsEventActive();
    void unknownCodePreservedWithGenericMessage();
    void hmiAlarmStartAndEnd();
    void hmiAlarmRestartEndsPrevious();
};

void AlarmEdgeDetectorTest::initTestCase()
{
    QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
             "QSQLITE driver not available");
}

void AlarmEdgeDetectorTest::cleanup()
{
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

void AlarmEdgeDetectorTest::zeroToNonZeroStartsEvent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("alarm_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);
        AlarmEdgeDetector det(&repo);

        QVERIFY(det.onPlcSnapshot(0, false, false, 1)); // adopt initial state
        QVERIFY(det.onPlcSnapshot(1, true, false, 2));  // 0 -> 1: start

        const auto active = repo.activeAlarm(AlarmSource::Plc);
        QVERIFY(active.has_value());
        QCOMPARE(active->code, quint16(1));
        QCOMPARE(active->messageSnapshot, QStringLiteral("急停"));
        QVERIFY(active->isActive());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

void AlarmEdgeDetectorTest::codeChangeEndsOldAndStartsNew()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("alarm_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);
        AlarmEdgeDetector det(&repo);

        QVERIFY(det.onPlcSnapshot(0, false, false, 1));
        QVERIFY(det.onPlcSnapshot(1, true, false, 2)); // start code 1
        QVERIFY(det.onPlcSnapshot(2, true, false, 3)); // code change: end 1, start 2

        const auto active = repo.activeAlarm(AlarmSource::Plc);
        QVERIFY(active.has_value());
        QCOMPARE(active->code, quint16(2));

        const QVector<AlarmEventRecord> all = repo.recentAlarms(10);
        QCOMPARE(all.size(), 2);
        // The first event is ended.
        QVERIFY(!all[1].isActive());
        QCOMPARE(all[1].code, quint16(1));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

void AlarmEdgeDetectorTest::zeroWithM14M4ClearEndsEvent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("alarm_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);
        AlarmEdgeDetector det(&repo);

        QVERIFY(det.onPlcSnapshot(0, false, false, 1));
        QVERIFY(det.onPlcSnapshot(1, true, false, 2)); // start
        QVERIFY(det.onPlcSnapshot(0, false, false, 3)); // D110=0, M14/M4 clear: end

        QVERIFY(!repo.activeAlarm(AlarmSource::Plc).has_value());
        const QVector<AlarmEventRecord> all = repo.recentAlarms(10);
        QCOMPARE(all.size(), 1);
        QVERIFY(!all[0].isActive());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

void AlarmEdgeDetectorTest::zeroWithM14OrM4SetKeepsEventActive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("alarm_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);
        AlarmEdgeDetector det(&repo);

        QVERIFY(det.onPlcSnapshot(0, false, false, 1));
        QVERIFY(det.onPlcSnapshot(1, true, false, 2)); // start
        // D110=0 but M14 still set: the event must stay active (spec §12).
        QVERIFY(det.onPlcSnapshot(0, true, false, 3));
        QVERIFY(repo.activeAlarm(AlarmSource::Plc).has_value());
        // M14 cleared, M4 still set: still active.
        QVERIFY(det.onPlcSnapshot(0, false, true, 4));
        QVERIFY(repo.activeAlarm(AlarmSource::Plc).has_value());
        // Both clear now: ends.
        QVERIFY(det.onPlcSnapshot(0, false, false, 5));
        QVERIFY(!repo.activeAlarm(AlarmSource::Plc).has_value());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

void AlarmEdgeDetectorTest::unknownCodePreservedWithGenericMessage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("alarm_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);
        AlarmEdgeDetector det(&repo);

        QVERIFY(det.onPlcSnapshot(0, false, false, 1));
        QVERIFY(det.onPlcSnapshot(0x1234, true, false, 2)); // unknown code

        const auto active = repo.activeAlarm(AlarmSource::Plc);
        QVERIFY(active.has_value());
        QCOMPARE(active->code, quint16(0x1234)); // original value preserved
        QCOMPARE(active->messageSnapshot, QStringLiteral("未知锁存故障"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

void AlarmEdgeDetectorTest::hmiAlarmStartAndEnd()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("alarm_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);
        AlarmEdgeDetector det(&repo);

        QVERIFY(det.startHmiAlarm(QStringLiteral("通讯中断"), AlarmSeverity::Critical, 1));
        auto active = repo.activeAlarm(AlarmSource::Hmi);
        QVERIFY(active.has_value());
        QCOMPARE(active->messageSnapshot, QStringLiteral("通讯中断"));
        QCOMPARE(active->source, AlarmSource::Hmi);

        QVERIFY(det.endHmiAlarm(2));
        QVERIFY(!repo.activeAlarm(AlarmSource::Hmi).has_value());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

void AlarmEdgeDetectorTest::hmiAlarmRestartEndsPrevious()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("alarm_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);
        AlarmEdgeDetector det(&repo);

        QVERIFY(det.startHmiAlarm(QStringLiteral("通讯中断"), AlarmSeverity::Critical, 1));
        QVERIFY(det.startHmiAlarm(QStringLiteral("数据库受限模式"), AlarmSeverity::Warning, 2));

        const auto active = repo.activeAlarm(AlarmSource::Hmi);
        QVERIFY(active.has_value());
        QCOMPARE(active->messageSnapshot, QStringLiteral("数据库受限模式"));
        // At most one active event per source (spec §12).
        const QVector<AlarmEventRecord> all = repo.recentAlarms(10);
        QCOMPARE(all.size(), 2);
        QVERIFY(!all[1].isActive());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("alarm_test"));
}

QTEST_GUILESS_MAIN(AlarmEdgeDetectorTest)
#include "test_alarm_edge_detector.moc"
