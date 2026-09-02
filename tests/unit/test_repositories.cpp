// Task 8 unit tests: recipe, settings and audit repositories (spec §12).
// Covers: recipe CRUD, unique name constraint, width CHECK constraint,
// settings upsert, audit append/query, and 365-day retention purge.

#include <QtTest>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "adapters/sqlite/sqlite_repositories.h"
#include "sqlite_test_util.h"

using namespace hlm;

class RepositoryTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void recipeInsertAndFind();
    void recipeUniqueNameConstraint();
    void recipeWidthCheckConstraint();
    void recipeUpdateAndDelete();
    void settingsUpsert();
    void auditAppendAndQuery();
    void auditPurgeBefore();
    void alarmPurgeEndedBeforeKeepsActive();
};

void RepositoryTest::initTestCase()
{
    QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
             "QSQLITE driver not available");
}

void RepositoryTest::cleanup()
{
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::recipeInsertAndFind()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteRecipeRepository repo(db);

        RecipeRecord r;
        r.name = QStringLiteral("A4");
        r.targetWidthMm = 210;
        r.createdBy = QStringLiteral("admin");
        r.updatedBy = QStringLiteral("admin");
        QString error;
        QVERIFY2(repo.saveRecipe(r, &error), qPrintable(error));

        const auto found = repo.findByName(QStringLiteral("A4"));
        QVERIFY(found.has_value());
        QCOMPARE(found->targetWidthMm, 210);
        QCOMPARE(found->createdBy, QStringLiteral("admin"));
        QVERIFY(found->createdAt.isValid());

        QCOMPARE(repo.allRecipes().size(), 1);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::recipeUniqueNameConstraint()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteRecipeRepository repo(db);

        RecipeRecord a;
        a.name = QStringLiteral("A4");
        a.targetWidthMm = 210;
        a.createdBy = a.updatedBy = QStringLiteral("admin");
        QString error;
        QVERIFY(repo.saveRecipe(a, &error));

        RecipeRecord b = a;
        b.id = -1;
        QVERIFY(!repo.saveRecipe(b, &error)); // UNIQUE constraint (spec §12)
        QVERIFY(error.contains(QStringLiteral("UNIQUE")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::recipeWidthCheckConstraint()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteRecipeRepository repo(db);

        RecipeRecord r;
        r.name = QStringLiteral("too-wide");
        r.targetWidthMm = 401; // outside 50-400 (spec §10.3, §12)
        r.createdBy = r.updatedBy = QStringLiteral("admin");
        QString error;
        QVERIFY(!repo.saveRecipe(r, &error));
        QVERIFY(error.contains(QStringLiteral("CHECK")));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::recipeUpdateAndDelete()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteRecipeRepository repo(db);

        RecipeRecord r;
        r.name = QStringLiteral("A4");
        r.targetWidthMm = 210;
        r.createdBy = r.updatedBy = QStringLiteral("admin");
        QString error;
        QVERIFY(repo.saveRecipe(r, &error));

        auto found = repo.findByName(QStringLiteral("A4"));
        QVERIFY(found.has_value());
        found->targetWidthMm = 250;
        found->updatedBy = QStringLiteral("admin");
        QVERIFY2(repo.saveRecipe(*found, &error), qPrintable(error));
        QCOMPARE(repo.findByName(QStringLiteral("A4"))->targetWidthMm, 250);

        QVERIFY(repo.deleteRecipe(found->id, &error));
        QVERIFY(!repo.findByName(QStringLiteral("A4")).has_value());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::settingsUpsert()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteSettingsRepository repo(db);

        SettingRecord s;
        s.key = QStringLiteral("serial.port");
        s.typedValue = QStringLiteral("COM3");
        s.updatedBy = QStringLiteral("admin");
        QString error;
        QVERIFY2(repo.setSetting(s, &error), qPrintable(error));

        auto got = repo.getSetting(QStringLiteral("serial.port"));
        QVERIFY(got.has_value());
        QCOMPARE(got->typedValue, QStringLiteral("COM3"));

        // Upsert overwrites the value.
        s.typedValue = QStringLiteral("COM4");
        QVERIFY(repo.setSetting(s, &error));
        QCOMPARE(repo.getSetting(QStringLiteral("serial.port"))->typedValue,
                 QStringLiteral("COM4"));
        QVERIFY(!repo.getSetting(QStringLiteral("missing")).has_value());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::auditAppendAndQuery()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteAuditRepository repo(db);

        AuditRecord a;
        a.occurredAt = QDateTime::currentDateTimeUtc();
        a.username = QStringLiteral("admin");
        a.role = Role::Admin;
        a.action = QStringLiteral("recipe.apply");
        a.target = QStringLiteral("A4");
        a.redactedParameters = QStringLiteral("width=210");
        a.result = AuditResult::Success;
        QString error;
        QVERIFY2(repo.append(a, &error), qPrintable(error));

        const QVector<AuditRecord> records = repo.recent(10);
        QCOMPARE(records.size(), 1);
        QCOMPARE(records[0].action, QStringLiteral("recipe.apply"));
        QCOMPARE(records[0].role, Role::Admin);
        QCOMPARE(records[0].result, AuditResult::Success);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::auditPurgeBefore()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteAuditRepository repo(db);

        AuditRecord old;
        old.occurredAt = QDateTime::currentDateTimeUtc().addDays(-400);
        old.username = QStringLiteral("admin");
        old.role = Role::Admin;
        old.action = QStringLiteral("auth.login");
        QString error;
        QVERIFY(repo.append(old, &error));

        AuditRecord fresh;
        fresh.occurredAt = QDateTime::currentDateTimeUtc();
        fresh.username = QStringLiteral("admin");
        fresh.role = Role::Admin;
        fresh.action = QStringLiteral("auth.login");
        QVERIFY(repo.append(fresh, &error));

        qint64 removed = 0;
        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-365);
        QVERIFY(repo.purgeBefore(cutoff, &removed, &error));
        QCOMPARE(removed, qint64(1));
        QCOMPARE(repo.recent(10).size(), 1);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

void RepositoryTest::alarmPurgeEndedBeforeKeepsActive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QSqlDatabase db = hlm_test::createMigratedDb(dir, QStringLiteral("repo_test"));
        QVERIFY(db.isOpen());
        SqliteAlarmRepository repo(db);

        // An old ended alarm.
        AlarmEventRecord ended;
        ended.source = AlarmSource::Plc;
        ended.code = 1;
        ended.messageSnapshot = QStringLiteral("急停");
        ended.severity = AlarmSeverity::Critical;
        ended.startedAt = QDateTime::currentDateTimeUtc().addDays(-400);
        ended.snapshotSequence = 1;
        QString error;
        const qint64 endedId = repo.startAlarm(ended, &error);
        QVERIFY(endedId >= 0);
        QVERIFY(repo.endAlarm(endedId, 2, &error));
        // Backdate the ended_at so the event is older than the 365-day window.
        QSqlQuery backdate(db);
        QVERIFY(backdate.exec(QStringLiteral(
            "UPDATE alarm_events SET ended_at = strftime('%Y-%m-%dT%H:%M:%f',"
            " 'now', '-400 days') WHERE id = %1").arg(endedId)));

        // An active alarm (never purged, spec §12).
        AlarmEventRecord active;
        active.source = AlarmSource::Plc;
        active.code = 2;
        active.messageSnapshot = QStringLiteral("安全门打开");
        active.severity = AlarmSeverity::Critical;
        active.startedAt = QDateTime::currentDateTimeUtc().addDays(-400);
        active.snapshotSequence = 3;
        const qint64 activeId = repo.startAlarm(active, &error);
        QVERIFY(activeId >= 0);

        qint64 removed = 0;
        const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-365);
        QVERIFY(repo.purgeEndedBefore(cutoff, &removed, &error));
        QCOMPARE(removed, qint64(1));

        const auto stillActive = repo.activeAlarm(AlarmSource::Plc);
        QVERIFY(stillActive.has_value());
        QCOMPARE(stillActive->id, activeId);
        QVERIFY(stillActive->isActive());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("repo_test"));
}

QTEST_GUILESS_MAIN(RepositoryTest)
#include "test_repositories.moc"
