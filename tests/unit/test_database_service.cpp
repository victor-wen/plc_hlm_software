// Task 8 unit tests: DatabaseService async facade (spec §7.3, §13).
//
// The service owns a dedicated worker thread and is moved onto it; the single
// QSqlDatabase connection is created and used only there. All operations run
// on the worker thread via queued invocation, and results arrive as signals on
// the caller's thread. These tests exercise that queued path end-to-end on a
// healthy database, and the restricted-mode fallback for an unopenable one
// (spec §13).

#include <QtTest>

#include <QFile>
#include <QMetaObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "adapters/sqlite/database_service.h"

using namespace hlm;

class DatabaseServiceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void healthyDatabaseEmitsReady();
    void healthyDatabaseUsesWalJournalMode();
    void healthyServiceRunsOperationsOnWorkerThread();
    void unopenableDatabaseEntersRestrictedMode();
    void restrictedServiceRejectsOperations();
    void needsInitialAdminSignal();
    void verifyPasswordSlot();
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
    QCOMPARE(service.thread(), QThread::currentThread());
}

// Spec §7.3 / WAL acceptance criterion: a healthy temp-file database must
// actually run in WAL journal mode, not silently fall back to rollback.
void DatabaseServiceTest::healthyDatabaseUsesWalJournalMode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService service(dir.filePath(QStringLiteral("app.db")));
    QSignalSpy readySpy(&service, &DatabaseService::ready);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);
    QVERIFY(!service.isRestricted());
    service.stop();

    // Re-open the file directly and confirm the persisted journal mode is WAL.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("wal_check"));
        db.setDatabaseName(dir.filePath(QStringLiteral("app.db")));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("PRAGMA journal_mode")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString().toLower(), QStringLiteral("wal"));
    }
    QSqlDatabase::removeDatabase(QStringLiteral("wal_check"));
}
// Drive admin creation and login through the queued invocation path so the
// production contract is exercised, not bypassed.
void DatabaseServiceTest::healthyServiceRunsOperationsOnWorkerThread()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService service(dir.filePath(QStringLiteral("app.db")));
    QSignalSpy readySpy(&service, &DatabaseService::ready);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);
    QVERIFY(!service.isRestricted());

    // The service object's thread affinity is the worker thread, which must
    // differ from the test thread (§7.3).
    QVERIFY(service.thread() != QThread::currentThread());

    // Plain C++ calls are automatically re-dispatched to the worker thread.
    // This is the production path used by Application callbacks.
    QSignalSpy adminSpy(&service, &DatabaseService::initialAdminCreated);
    service.createInitialAdmin(QStringLiteral("admin"), QStringLiteral("s3cret!"));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
    QCOMPARE(adminSpy[0][0].toBool(), true);

    QSignalSpy loginSpy(&service, &DatabaseService::loginResult);
    service.login(QStringLiteral("admin"), QStringLiteral("s3cret!"));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.size() > 0, 5000);
    QVERIFY(loginSpy[0][0].value<LoginResult>().ok);

    // A wrong password still returns a clean failure through the same path.
    QSignalSpy badLoginSpy(&service, &DatabaseService::loginResult);
    service.login(QStringLiteral("admin"), QStringLiteral("wrong"));
    QTRY_VERIFY_WITH_TIMEOUT(badLoginSpy.size() > 0, 5000);
    QCOMPARE(badLoginSpy[0][0].value<LoginResult>().ok, false);

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

// needsInitialAdmin: an empty database reports true, and after the initial
// admin is created the same slot reports false (spec §11.5 first start).
void DatabaseServiceTest::needsInitialAdminSignal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService service(dir.filePath(QStringLiteral("app.db")));
    QSignalSpy readySpy(&service, &DatabaseService::ready);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);
    QVERIFY(!service.isRestricted());

    QSignalSpy needsSpy(&service, &DatabaseService::initialAdminNeeded);
    const bool queued = QMetaObject::invokeMethod(
        &service, "needsInitialAdmin", Qt::QueuedConnection);
    QVERIFY(queued);
    QTRY_VERIFY_WITH_TIMEOUT(needsSpy.size() > 0, 5000);
    QCOMPARE(needsSpy[0][0].toBool(), true);

    QSignalSpy adminSpy(&service, &DatabaseService::initialAdminCreated);
    QVERIFY(QMetaObject::invokeMethod(
        &service, "createInitialAdmin", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
    QCOMPARE(adminSpy[0][0].toBool(), true);

    QSignalSpy needsAfterSpy(&service, &DatabaseService::initialAdminNeeded);
    QVERIFY(QMetaObject::invokeMethod(
        &service, "needsInitialAdmin", Qt::QueuedConnection));
    QTRY_VERIFY_WITH_TIMEOUT(needsAfterSpy.size() > 0, 5000);
    QCOMPARE(needsAfterSpy[0][0].toBool(), false);

    service.stop();
}

// verifyPassword slot: queued invocation returns the correct result for a
// known user (true with the right password, false with a wrong one).
void DatabaseServiceTest::verifyPasswordSlot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DatabaseService service(dir.filePath(QStringLiteral("app.db")));
    QSignalSpy readySpy(&service, &DatabaseService::ready);
    service.start();
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.size() > 0, 5000);
    QVERIFY(!service.isRestricted());

    QSignalSpy adminSpy(&service, &DatabaseService::initialAdminCreated);
    QVERIFY(QMetaObject::invokeMethod(
        &service, "createInitialAdmin", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.size() > 0, 5000);
    QCOMPARE(adminSpy[0][0].toBool(), true);

    // Fetch the admin's id via listUsers.
    QSignalSpy usersSpy(&service, &DatabaseService::usersLoaded);
    QVERIFY(QMetaObject::invokeMethod(
        &service, "listUsers", Qt::QueuedConnection));
    QTRY_VERIFY_WITH_TIMEOUT(usersSpy.size() > 0, 5000);
    const QVector<UserRecord> users = usersSpy[0][0].value<QVector<UserRecord>>();
    QCOMPARE(users.size(), 1);
    const qint64 adminId = users[0].id;

    QSignalSpy okSpy(&service, &DatabaseService::passwordVerified);
    QVERIFY(QMetaObject::invokeMethod(
        &service, "verifyPassword", Qt::QueuedConnection,
        Q_ARG(qint64, adminId), Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(okSpy.size() > 0, 5000);
    QCOMPARE(okSpy[0][0].toBool(), true);

    QSignalSpy badSpy(&service, &DatabaseService::passwordVerified);
    QVERIFY(QMetaObject::invokeMethod(
        &service, "verifyPassword", Qt::QueuedConnection,
        Q_ARG(qint64, adminId), Q_ARG(QString, QStringLiteral("wrong"))));
    QTRY_VERIFY_WITH_TIMEOUT(badSpy.size() > 0, 5000);
    QCOMPARE(badSpy[0][0].toBool(), false);

    service.stop();
}

QTEST_GUILESS_MAIN(DatabaseServiceTest)
#include "test_database_service.moc"
