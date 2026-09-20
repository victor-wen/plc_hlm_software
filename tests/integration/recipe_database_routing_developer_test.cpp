// PLC-HMI-002 developer-owned integration/regression tests (D8) for the
// composition-root routing of page-local recipe database results.
//
// Exercises the real Application wiring: RecipeWidthPage -> DatabaseService ->
// page. Before the implementation Application discarded recipeSaved/recipeDeleted
// (bool, error) and only re-listed, so no page status was ever produced.
//
// The page UI permission gating is covered by unit tests; these tests emit the
// page's request signals directly to isolate the Application route.

#include <QtTest>

#include <QApplication>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "app/application.h"
#include "app/configuration.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

struct StartedApp
{
    QTemporaryDir dir;
    AppConfig cfg;

    StartedApp()
    {
        cfg.useSimulatedGateway = true;
        cfg.simulatedTickIntervalMs = 0;
        cfg.databasePath = dir.filePath(QStringLiteral("app.db"));
    }
};

void startAndWaitReady(Application &app, DatabaseService *&db)
{
    db = app.database();
    QVERIFY(db != nullptr);

    // Subscribe before start(): in an optimized Release build the database
    // worker can open the temporary database and emit recipesLoaded before
    // start() returns. Connecting afterwards races that one-shot startup
    // signal and leaves this helper waiting for an event that already happened.
    QSignalSpy readySpy(db, &DatabaseService::recipesLoaded);

    app.start();

    // `isRestricted()` is false before the worker opens the database, so wait
    // for the startup listRecipes() in Application::onReady, which only runs
    // on the ready() path.
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 5000);
}

// The page only dispatches recipe mutations for a logged-in administrator, and
// Application records the session username as created_by, so the routing test
// follows the same bootstrap as the production first-run flow.
void loginAsAdmin(DatabaseService *db)
{
    QSignalSpy adminSpy(db, &DatabaseService::initialAdminCreated);
    QVERIFY(QMetaObject::invokeMethod(
        db, "createInitialAdmin", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(adminSpy.count() > 0, 10000);
    QCOMPARE(adminSpy[0][0].toBool(), true);

    QSignalSpy loginSpy(db, &DatabaseService::loginResult);
    QVERIFY(QMetaObject::invokeMethod(
        db, "login", Qt::QueuedConnection,
        Q_ARG(QString, QStringLiteral("admin")),
        Q_ARG(QString, QStringLiteral("s3cret!"))));
    QTRY_VERIFY_WITH_TIMEOUT(loginSpy.count() > 0, 10000);
    QVERIFY(loginSpy[0][0].value<LoginResult>().ok);
}

} // namespace

class RecipeDatabaseRoutingDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void saveAndDeleteResultsRouteToPageAndReloadList();
    void failureResultsRouteErrorDetailToPage();
};

void RecipeDatabaseRoutingDeveloperTest::saveAndDeleteResultsRouteToPageAndReloadList()
{
    StartedApp started;
    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);
    loginAsAdmin(db);
    auto *page = app.window()->findChild<RecipeWidthPage *>();
    QVERIFY(page != nullptr);

    QSignalSpy savedSpy(db, &DatabaseService::recipeSaved);
    emit page->saveRecipeRequested(QStringLiteral("路由配方"), 200);

    // Pending is immediately visible and never claims success (D2).
    QVERIFY2(!page->statusText().trimmed().isEmpty(),
             "the save request produced no visible pending status");
    QVERIFY2(!page->statusText().contains(QStringLiteral("已保存")),
             "a pending save must not claim success");

    QTRY_COMPARE_WITH_TIMEOUT(savedSpy.count(), 1, 5000);
    QVERIFY2(savedSpy[0][0].toBool(),
             qPrintable(QStringLiteral("save reported failure: '%1'")
                            .arg(savedSpy[0][1].toString())));

    // The success result is routed into the page and survives the follow-up
    // list reload (D2).
    QTRY_COMPARE_WITH_TIMEOUT(page->recipeList()->count(), 1, 5000);
    QVERIFY2(page->statusText().contains(QStringLiteral("已保存")),
             qPrintable(QStringLiteral("status was '%1'")
                            .arg(page->statusText())));

    // Delete the saved record through the same page -> database route (D3).
    const qint64 id =
        page->recipeList()->item(0)->data(Qt::UserRole).toLongLong();
    QVERIFY(id >= 0);
    page->recipeList()->setCurrentRow(0);
    QApplication::processEvents();
    QVERIFY(!page->nameEdit()->text().isEmpty());

    QSignalSpy deletedSpy(db, &DatabaseService::recipeDeleted);
    emit page->deleteRecipeRequested(id);
    QVERIFY2(!page->statusText().contains(QStringLiteral("已删除")),
             "a pending delete must not claim success");
    QVERIFY2(!page->statusText().trimmed().isEmpty(),
             "the delete request produced no visible pending status");

    QTRY_COMPARE_WITH_TIMEOUT(deletedSpy.count(), 1, 5000);
    QCOMPARE(deletedSpy[0][0].toBool(), true);
    QTRY_COMPARE_WITH_TIMEOUT(page->recipeList()->count(), 0, 5000);
    QVERIFY(page->statusText().contains(QStringLiteral("已删除")));

    // Confirmed deletion: the reload cleared the selection and the editors
    // while the result text stayed visible (D1 + D3).
    QCOMPARE(page->recipeList()->currentRow(), -1);
    QVERIFY(page->nameEdit()->text().isEmpty());

    app.shutdown();
}

void RecipeDatabaseRoutingDeveloperTest::failureResultsRouteErrorDetailToPage()
{
    StartedApp started;
    Application app(started.cfg);
    DatabaseService *db = nullptr;
    startAndWaitReady(app, db);
    loginAsAdmin(db);
    auto *page = app.window()->findChild<RecipeWidthPage *>();
    QVERIFY(page != nullptr);

    QSignalSpy savedSpy(db, &DatabaseService::recipeSaved);
    // Width 0 violates the schema CHECK constraint: the database reports a
    // real failure without any permission or PLC involvement.
    emit page->saveRecipeRequested(QStringLiteral("坏宽度配方"), 0);
    QVERIFY(!page->statusText().contains(QStringLiteral("已保存")));

    QTRY_COMPARE_WITH_TIMEOUT(savedSpy.count(), 1, 5000);
    QCOMPARE(savedSpy[0][0].toBool(), false);
    const QString saveError = savedSpy[0][1].toString();
    QVERIFY2(!saveError.isEmpty(), "expected a non-empty database error");
    QTRY_VERIFY2(page->statusText().contains(saveError),
                 qPrintable(QStringLiteral("save failure detail missing: '%1'")
                                .arg(page->statusText())));
    QVERIFY(page->statusText().contains(QStringLiteral("保存失败")));

    QSignalSpy deletedSpy(db, &DatabaseService::recipeDeleted);
    emit page->deleteRecipeRequested(999999);
    QTRY_COMPARE_WITH_TIMEOUT(deletedSpy.count(), 1, 5000);
    QCOMPARE(deletedSpy[0][0].toBool(), false);
    const QString deleteError = deletedSpy[0][1].toString();
    QVERIFY2(!deleteError.isEmpty(), "expected a non-empty database error");
    QTRY_VERIFY2(page->statusText().contains(deleteError),
                 qPrintable(QStringLiteral("delete failure detail missing: '%1'")
                                .arg(page->statusText())));
    QVERIFY(page->statusText().contains(QStringLiteral("删除失败")));
    QVERIFY(!page->statusText().contains(QStringLiteral("已删除")));

    app.shutdown();
}

QTEST_MAIN(RecipeDatabaseRoutingDeveloperTest)
#include "recipe_database_routing_developer_test.moc"

