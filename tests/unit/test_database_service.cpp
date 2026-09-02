// Task 8 unit tests: DatabaseService restricted mode (spec §13).
// A database that cannot be opened or migrated must put the service into
// restricted mode; the application then keeps only online-stop and
// software-estop (enforced by the application layer, spec §13).

#include <QtTest>

#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QTemporaryDir>

#include "adapters/sqlite/database_service.h"

using namespace hlm;

class DatabaseServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void healthyDatabaseEmitsReady();
    void unopenableDatabaseEntersRestrictedMode();
    void restrictedServiceRejectsOperations();
};

void DatabaseServiceTest::initTestCase()
{
    QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
             "QSQLITE driver not available");
}

void DatabaseServiceTest::healthyDatabaseEmitsReady()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService service(dir.filePath(QStringLiteral("app.db")));
    QSignalSpy readySpy(&service, &DatabaseService::ready);
    QSignalSpy restrictedSpy(&service, &DatabaseService::databaseRestricted);
    service.start();
    // The worker thread may emit before wait() is called; QTRY_VERIFY spins
    // the event loop and re-checks, so an already-queued signal is seen.
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);
    QCOMPARE(restrictedSpy.size(), 0);
    QVERIFY(!service.isRestricted());
    service.stop();
}

void DatabaseServiceTest::unopenableDatabaseEntersRestrictedMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A path inside a non-existent directory cannot be opened.
    DatabaseService service(dir.filePath(QStringLiteral("no/such/dir/app.db")));
    QSignalSpy readySpy(&service, &DatabaseService::ready);
    QSignalSpy restrictedSpy(&service, &DatabaseService::databaseRestricted);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(restrictedSpy.size() > 0, 5000);
    QCOMPARE(readySpy.size(), 0);
    QVERIFY(service.isRestricted());
    service.stop();
}

void DatabaseServiceTest::restrictedServiceRejectsOperations()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService service(dir.filePath(QStringLiteral("no/such/dir/app.db")));
    QSignalSpy restrictedSpy(&service, &DatabaseService::databaseRestricted);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(restrictedSpy.size() > 0, 5000);

    // Operations must fail cleanly instead of crashing (spec §13).
    QSignalSpy adminSpy(&service, &DatabaseService::initialAdminCreated);
    service.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("x"));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
    QCOMPARE(adminSpy[0][0].toBool(), false);

    QSignalSpy loginSpy(&service, &DatabaseService::loginResult);
    service.login(QStringLiteral("admin"), QStringLiteral("x"));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.size() > 0, 5000);
    QCOMPARE(loginSpy[0][0].value<LoginResult>().ok, false);

    service.stop();
}

QTEST_GUILESS_MAIN(DatabaseServiceTest)
#include "test_database_service.moc"
