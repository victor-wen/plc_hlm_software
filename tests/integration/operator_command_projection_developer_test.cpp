// PLC-HMI-001 developer integration tests (D12): the real composition root
// projects the operator-command lifecycle into ShellModel. Complements (never
// replaces) operator_command_application_test.cpp.

#include <QtTest>

#include <QSpinBox>
#include <QTemporaryDir>

#include <memory>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "domain/operator_command_status.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"

using namespace hlm;

namespace {

// Protocol addresses (0-based, matching the centralized AddressTable).
constexpr quint16 kM103 = 103;
constexpr quint16 kM104 = 104;
constexpr quint16 kD130 = 130;

void homeReady(SimulatedPlcGateway &gw)
{
    gw.writeCoil(kM103, true);
    gw.writeCoil(kM103, false);
    gw.tick();
    gw.tick(); // home return takes 2 s
}

struct WarningCapture
{
    QVector<QtMsgType> types;
    QVector<QString> messages;
};

WarningCapture g_capture;
QtMessageHandler g_previousHandler = nullptr;

void captureMessages(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    g_capture.types.append(type);
    g_capture.messages.append(message);
}

bool capturedWarning()
{
    for (QtMsgType type : g_capture.types) {
        if (type == QtWarningMsg)
            return true;
    }
    return false;
}

// Runs the application on the in-process simulator with an isolated database.
Application *startSimulatedApplication(const QTemporaryDir &dir, SimulatedPlcGateway **gwOut)
{
    AppConfig cfg;
    cfg.useSimulatedGateway = true;
    cfg.simulatedTickIntervalMs = 0;
    cfg.databasePath = dir.filePath(QStringLiteral("app.db"));

    auto *app = new Application(cfg);
    app->start();
    auto *gw = qobject_cast<SimulatedPlcGateway *>(app->gateway());
    if (gwOut != nullptr)
        *gwOut = gw;
    return app;
}

} // namespace

class OperatorCommandProjectionDeveloperTest : public QObject
{
    Q_OBJECT

private slots:
    void commandGenerationIncrementsPerUserRequest();
    void unhandledCommandProducesWarningAndRejectedStatus();
    void adjustTerminalVerdictIsNotOverriddenBySuccessLikeSnapshot();
};

// D6: the projection counts each user request once; pending updates keep the
// generation of their accepted request, and the optional correlation fields
// stay empty/0 until PLC-HMI-003.
void OperatorCommandProjectionDeveloperTest::commandGenerationIncrementsPerUserRequest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());

    ShellModel *shell = app->shell();
    ControlCoordinator *coord = app->coordinator();
    QVERIFY(shell != nullptr);
    QVERIFY(coord != nullptr);
    coord->setRole(Role::Anonymous);

    QVector<OperatorCommandStatus> statuses;
    connect(shell, &ShellModel::operatorCommandStatusChanged, this,
            [&statuses](const OperatorCommandStatus &s) { statuses.append(s); });

    // Rejected user request: visible reason and a fresh generation.
    coord->reset();
    QCOMPARE(statuses.size(), 1);
    QVERIFY(statuses[0].lifecycle_state == OperatorCommandState::Rejected);
    QVERIFY(!statuses[0].human_readable_detail.isEmpty());
    const quint64 rejectedGeneration = statuses[0].command_generation;
    QVERIFY(rejectedGeneration > 0);

    // Accepted request: accepted + pending share one new generation.
    coord->stop();
    QCOMPARE(statuses.size(), 3);
    QVERIFY(statuses[1].lifecycle_state == OperatorCommandState::Accepted);
    QVERIFY(statuses[2].lifecycle_state == OperatorCommandState::Pending);
    QVERIFY(!statuses[2].human_readable_detail.isEmpty());
    QVERIFY(statuses[1].command_generation > rejectedGeneration);
    QCOMPARE(statuses[2].command_generation, statuses[1].command_generation);

    // Duplicate/in-progress request: visibly rejected, generation increments.
    coord->stop();
    QCOMPARE(statuses.size(), 4);
    QVERIFY(statuses[3].lifecycle_state == OperatorCommandState::Rejected);
    QVERIFY(!statuses[3].human_readable_detail.isEmpty());
    QVERIFY(statuses[3].command_generation > statuses[1].command_generation);

    // No request-id/gateway-generation correlation is fabricated in this phase.
    QVERIFY(!statuses[3].request_id.has_value());
    QCOMPARE(statuses[3].gateway_generation, quint64(0));

    app->shutdown();
}

// D11: an unhandled command routed from the action bar emits a Qt warning and
// a visible rejected diagnostic with detail.
void OperatorCommandProjectionDeveloperTest::unhandledCommandProducesWarningAndRejectedStatus()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    ActionBar *bar = app->window()->findChild<ActionBar *>();
    QVERIFY(bar != nullptr);

    g_capture = {};
    g_previousHandler = qInstallMessageHandler(captureMessages);
    emit bar->actionRequested(static_cast<Command>(9999));
    qInstallMessageHandler(g_previousHandler);

    QVERIFY2(capturedWarning(), "unhandled command must emit a Qt warning");
    const OperatorCommandStatus status = app->shell()->operatorCommandStatus();
    QVERIFY(status.lifecycle_state == OperatorCommandState::Rejected
            || status.lifecycle_state == OperatorCommandState::Failed);
    QVERIFY2(!status.human_readable_detail.isEmpty(),
             "unhandled command diagnostic must carry a human-readable detail");
    QVERIFY(status.command_generation > 0);

    app->shutdown();
}

// D9/D6: the coordinator terminal adjust result is projected to the shell and
// later success-like snapshots can never override it.
void OperatorCommandProjectionDeveloperTest::adjustTerminalVerdictIsNotOverriddenBySuccessLikeSnapshot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SimulatedPlcGateway *gw = nullptr;
    std::unique_ptr<Application> app(startSimulatedApplication(dir, &gw));
    QVERIFY(gw != nullptr);

    for (int i = 0; i < 3 && !gw->isOnline(); ++i)
        gw->tick();
    QVERIFY(gw->isOnline());
    homeReady(*gw);
    app->coordinator()->setRole(Role::Admin);

    RecipeWidthPage *page = app->window()->findChild<RecipeWidthPage *>();
    QVERIFY(page != nullptr);

    // Stall the motor so the coordinator converges to a failure.
    gw->model().setPositioningStall(true);
    emit page->applyAdjustRequested(300);
    QVERIFY(gw->model().readRegister(128) == quint16(300));
    gw->tick();
    for (int i = 0; i < 14; ++i)
        gw->tick();

    const OperatorCommandStatus terminal = app->shell()->operatorCommandStatus();
    QVERIFY(isTerminal(terminal.lifecycle_state));
    QVERIFY(terminal.lifecycle_state == OperatorCommandState::TimedOut
            || terminal.lifecycle_state == OperatorCommandState::Failed);
    QVERIFY(!terminal.human_readable_detail.isEmpty());
    QVERIFY(!page->statusText().contains(QStringLiteral("调宽成功")));

    // Later success-like snapshots must not change the verdict or the detail.
    gw->model().writeCoil(45, false);      // M45 off
    gw->model().writeCoil(34, false);      // M34 off
    gw->model().writeRegister(kD130, 300); // D130 == applied target
    gw->model().writeCoil(44, true);       // M44 on
    gw->tick();

    const OperatorCommandStatus afterSnapshot = app->shell()->operatorCommandStatus();
    QCOMPARE(afterSnapshot.lifecycle_state, terminal.lifecycle_state);
    QCOMPARE(afterSnapshot.human_readable_detail, terminal.human_readable_detail);
    QVERIFY2(!page->statusText().contains(QStringLiteral("调宽成功")),
             qPrintable(QStringLiteral("success-like snapshot overrode the "
                                       "coordinator verdict: %1")
                            .arg(page->statusText())));

    app->shutdown();
}

QTEST_MAIN(OperatorCommandProjectionDeveloperTest)
#include "operator_command_projection_developer_test.moc"
