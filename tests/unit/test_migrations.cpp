// Task 8 unit tests: schema migrations (spec §12, §13).
// Covers: fresh migration, idempotent re-run, checksum tamper detection,
// and rollback of a failing migration (restricted-mode precondition).

#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "adapters/sqlite/migration_runner.h"
#include "sqlite_test_util.h"

using namespace hlm;

class MigrationRunnerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void freshDatabaseGetsAllTables();
    void rerunIsIdempotent();
    void tamperedMigrationChecksumDetected();
    void failingMigrationRollsBack();
};

void MigrationRunnerTest::initTestCase()
{
    QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
             "QSQLITE driver not available");
}

void MigrationRunnerTest::cleanupTestCase()
{
    QSqlDatabase::removeDatabase(QStringLiteral("mig_test"));
}

void MigrationRunnerTest::freshDatabaseGetsAllTables()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("mig_test"));
        QVERIFY2(db.isOpen(), qPrintable(db.lastError().text()));

        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")));
        QStringList tables;
        while (q.next())
            tables << q.value(0).toString();
        for (const QString &t : {QStringLiteral("schema_migrations"),
                                 QStringLiteral("users"),
                                 QStringLiteral("recipes"),
                                 QStringLiteral("alarm_events"),
                                 QStringLiteral("audit_log"),
                                 QStringLiteral("app_settings")}) {
            QVERIFY2(tables.contains(t), qPrintable(QStringLiteral("missing table ") + t));
        }

        QSqlQuery v(db);
        QVERIFY(v.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")));
        QVERIFY(v.next());
        QCOMPARE(v.value(0).toInt(), schemaMigrations().last().version);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mig_test"));
}

void MigrationRunnerTest::rerunIsIdempotent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("mig_test"));
        QVERIFY(db.isOpen());
        QString error;
        QVERIFY2(runMigrations(db, schemaMigrations(), &error), qPrintable(error));
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM schema_migrations")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), schemaMigrations().size());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mig_test"));
}

void MigrationRunnerTest::tamperedMigrationChecksumDetected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("mig_test"));
        QVERIFY(db.isOpen());
        // Tamper with the recorded checksum of migration 1.
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral(
            "UPDATE schema_migrations SET checksum = 'deadbeef' WHERE version = 1")));
        QString error;
        QVERIFY(!runMigrations(db, schemaMigrations(), &error));
        QVERIFY(error.contains(QStringLiteral("checksum")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mig_test"));
}

void MigrationRunnerTest::failingMigrationRollsBack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("mig_test"));
        QVERIFY(db.isOpen());
        // A migration that creates a table then fails on a second statement
        // must roll back the whole transaction (spec §13).
        const QVector<Migration> bad = {
            {99, QStringLiteral("bad"),
             QStringLiteral("CREATE TABLE should_rollback (id INTEGER);"
                            " CREATE TABLE should_rollback (id INTEGER);")},
        };
        QString error;
        QVERIFY(!runMigrations(db, bad, &error));
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE name='should_rollback'")));
        QVERIFY(!q.next()); // rolled back: table must not exist
        QSqlQuery v(db);
        QVERIFY(v.exec(QStringLiteral("SELECT COUNT(*) FROM schema_migrations"
                                      " WHERE version = 99")));
        QVERIFY(v.next());
        QCOMPARE(v.value(0).toInt(), 0); // no record of the failed migration
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mig_test"));
}

QTEST_GUILESS_MAIN(MigrationRunnerTest)
#include "test_migrations.moc"
