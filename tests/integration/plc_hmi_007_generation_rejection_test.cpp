// PLC-HMI-007 wave-2 black-box integration tests: application-level gateway
// generation filtering through the frozen S-INJECT seam (brief OB-6).
//
// Authored only from .ai/test-briefs/PLC-HMI-007.yaml (brief_version 2), the
// approved .ai/project-contract.yaml (IPlcGateway lifecycle: "Application
// increments gateway_generation before replacement, disconnects old signals,
// and ignores later events from older generations"; F-12) and inspectable test
// sources under tests/**. No production implementation source was read.
//
// Frozen seam S-INJECT (brief): AppConfig::plcGateway of type IPlcGateway*
// (default nullptr). When non-null, Application uses that instance as its
// initial gateway, assigns it the generation counter value (1 for the first),
// and applies the same obsolete-generation rejection to every event. The
// instance is caller-owned (like AppConfig::serialPortDiscovery).
//
// Observable surface used (every symbol appears in inspectable test sources
// under tests/**):
//   * Application/AppConfig: start/shutdown/gateway/window/coordinator, the
//     useSimulatedGateway/simulatedTickIntervalMs/databasePath configuration
//     and the injected-gateway pattern;
//   * SimulatedPlcGateway port signals snapshotReady(quint64, DeviceSnapshot)
//     and connectionStateChanged(quint64, bool), plus the public
//     gatewayGeneration() accessor (brief S-SIM);
//   * shell-driven enabling, established by
//     tests/unit/visible_disabled_reasons_test.cpp and the page-test fixtures:
//     a fresh/offline anonymous shell has zero enabled action-bar controls, an
//     online anonymous session keeps Stop/software-estop enabled, and an
//     administrator with connected/homed/manual data may use the recipe apply
//     control while a disconnected/not-homed/stale state disables it;
//   * DeviceSnapshotData/DeviceSnapshot construction with the contract-fixed
//     quality/age members.
//
// Test-local emitter for the inherited port signals: Qt signals are emitted
// from a derived class. If the compiler rejects this pattern on this tree, the
// recorded fallback is QMetaObject::invokeMethod on the signal with Q_ARG
// (see the wave-2 report).
//
// Expected RED: API-missing compile failure until the developer implements
// S-INJECT exactly as frozen (AppConfig::plcGateway). Once the seam builds, the
// obsolete-generation cases are expected to be runtime RED until Application
// rejects older-generation events at its event slots.

#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QTemporaryDir>

#include "adapters/simulator/simulated_plc_gateway.h"
#include "app/application.h"
#include "app/configuration.h"
#include "application/control_coordinator.h"
#include "domain/device_snapshot.h"
#include "ports/iplc_gateway.h"
#include "ui/MainWindow.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/shell/action_bar.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/permission_button.h"

using namespace hlm;

namespace {

// Frozen S-INJECT: the first gateway generation is 1; an obsolete event carries
// an older generation (0).
constexpr quint64 kCurrentGeneration = 1;
constexpr quint64 kObsoleteGeneration = 0;

// Settles the event loop without advancing the injected gateway.
void settle(int milliseconds = 100)
{
    QTest::qWait(milliseconds);
    QApplication::processEvents();
}

// Test-local emitter for the inherited SimulatedPlcGateway port signals. It
// adds no production API: every helper only emits an existing inherited signal
// with the generation the test wants to exercise.
class EmittingGateway : public SimulatedPlcGateway
{
public:
    using SimulatedPlcGateway::SimulatedPlcGateway;

    void emitSnapshot(quint64 generation, const DeviceSnapshot &snapshot)
    {
        emit snapshotReady(generation, snapshot);
    }

    void emitConnection(quint64 generation, bool online)
    {
        emit connectionStateChanged(generation, online);
    }
};

// Valid administrator-operable machine data: connected, manual (M1),
// homed (M9 per COMMMAP), all quality blocks valid. Same shape as the
// inspectable page-test fixtures.
DeviceSnapshotData validHomedManualSnapshot()
{
    DeviceSnapshotData d;
    d.connected = true;
    d.statusWord1 = (quint16(1) << 1) | (quint16(1) << 9); // M1 manual, M9 homed
    d.statusWord3 = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = 128;
    d.widthSpeed = 15;
    d.beltSpeed = 5000;
    d.heartbeat = 1;
    d.fast_quality = DataQuality::Valid;
    d.home_quality = DataQuality::Valid;
    d.command_quality = DataQuality::Valid;
    d.slow_quality = DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
    return d;
}

// Machine data that must disable the recipe apply control for an administrator
// if it is accepted: disconnected, not homed and every block stale.
DeviceSnapshotData disabledSnapshot()
{
    DeviceSnapshotData d;
    d.connected = false;
    d.statusWord1 = 0;
    d.statusWord3 = 0;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = 50;
    d.pulsePerMm = 128;
    d.widthSpeed = 15;
    d.beltSpeed = 5000;
    d.heartbeat = 0;
    d.fast_quality = DataQuality::Stale;
    d.home_quality = DataQuality::Stale;
    d.command_quality = DataQuality::Stale;
    d.slow_quality = DataQuality::Stale;
    d.overall_quality = aggregateQuality(d);
    return d;
}

int enabledActionBarControls(Application &app)
{
    if (app.window() == nullptr)
        return -1;
    ActionBar *bar = app.window()->findChild<ActionBar *>();
    if (bar == nullptr)
        return -1;
    int enabled = 0;
    for (QAbstractButton *button : bar->findChildren<QAbstractButton *>()) {
        if (button->isEnabled())
            ++enabled;
    }
    return enabled;
}

} // namespace

class PlcHmi007GenerationRejectionTest : public QObject
{
    Q_OBJECT

private slots:
    // --- S-INJECT: the injected gateway is used and current events are accepted
    void injectedGatewayIsUsedAndCurrentGenerationEventsAreAccepted();

    // --- OB-6: obsolete-generation events are rejected -------------------------
    void obsoleteGenerationEventsAreRejectedWhileCurrentGenerationEventsAreAccepted();

    // --- OB-6: the default production composition path is unchanged ------------
    void defaultCompositionStillCreatesItsOwnGateway();
};

// --- S-INJECT ------------------------------------------------------------------

void PlcHmi007GenerationRejectionTest::injectedGatewayIsUsedAndCurrentGenerationEventsAreAccepted()
{
    // The frozen seam: a non-null AppConfig::plcGateway is the application's
    // initial gateway, gets the first generation value, and current-generation
    // events are accepted (observable through the shell enabling).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    EmittingGateway injected;
    AppConfig cfg;
    cfg.plcGateway = &injected;
    cfg.useSimulatedGateway = false;
    cfg.simulatedTickIntervalMs = 0;
    cfg.databasePath = dir.filePath(QStringLiteral("app.db"));

    Application app(cfg);
    app.start();
    QVERIFY2(app.window() != nullptr, "the composed application must expose its window");
    app.window()->show();
    QApplication::processEvents();

    QVERIFY2(app.gateway() == &injected,
             "S-INJECT: a non-null AppConfig::plcGateway must be used as the initial "
             "gateway instead of composing one");
    QCOMPARE(injected.gatewayGeneration(), kCurrentGeneration);

    QVERIFY2(app.coordinator() != nullptr, "the composed application must expose the coordinator");
    // Owner correction (2026-09-17): the action bar and the recipe page read the
    // shell-side session (ShellModel::role()), which Application only syncs on a
    // real login; a fresh temp DB stays Anonymous. The established inspectable
    // seam is tests/integration/restricted_routing_developer_test.cpp:437.
    app.shell()->setUser(QString(), Role::Anonymous);

    // Precondition: a fresh, offline shell offers no anonymous action-bar
    // control (offline Stop/estop are refused, spec 10.5/10.6).
    QVERIFY2(app.window()->findChild<ActionBar *>() != nullptr,
             "the composed shell has no action bar");
    QCOMPARE(enabledActionBarControls(app), 0);

    // Current-generation acceptance: a connection event and a snapshot with the
    // current generation must bring the shell online.
    injected.emitConnection(kCurrentGeneration, true);
    injected.emitSnapshot(kCurrentGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    QTRY_VERIFY_WITH_TIMEOUT(enabledActionBarControls(app) > 0, 2000);

    // Current-generation snapshot acceptance on the administrator recipe
    // surface: connected, homed, manual data enables the apply control. The
    // recipe page reads the shell session role (owner correction 2026-09-17:
    // the seam is app.shell()->setUser, not coordinator()->setRole).
    app.shell()->setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage *recipe = app.window()->findChild<RecipeWidthPage *>();
    QVERIFY2(recipe != nullptr, "the composed shell has no recipe width page");
    QVERIFY2(recipe->applyButton() != nullptr, "the recipe page has no apply control");
    injected.emitSnapshot(kCurrentGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    QTRY_VERIFY_WITH_TIMEOUT(recipe->applyButton()->isEnabled(), 2000);

    app.shutdown();
}

// --- OB-6 ----------------------------------------------------------------------

void PlcHmi007GenerationRejectionTest::obsoleteGenerationEventsAreRejectedWhileCurrentGenerationEventsAreAccepted()
{
    // Brief OB-6: an obsolete-generation connection or snapshot must not change
    // the shell/online state, while the same events with the current generation
    // are accepted. Every rejection assertion is bracketed by an observable
    // acceptance control so a rejected event can never pass by being ignored.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    EmittingGateway injected;
    AppConfig cfg;
    cfg.plcGateway = &injected;
    cfg.useSimulatedGateway = false;
    cfg.simulatedTickIntervalMs = 0;
    cfg.databasePath = dir.filePath(QStringLiteral("app.db"));

    Application app(cfg);
    app.start();
    QVERIFY2(app.window() != nullptr, "the composed application must expose its window");
    app.window()->show();
    QApplication::processEvents();
    QVERIFY2(app.gateway() == &injected,
             "S-INJECT: the injected gateway must be the application gateway");

    QVERIFY2(app.coordinator() != nullptr, "the composed application must expose the coordinator");
    // Owner correction (2026-09-17): the action bar reads the shell session
    // role (ShellModel), which Application only syncs on a real login; a fresh
    // temp DB stays Anonymous. The established inspectable seam is
    // tests/integration/restricted_routing_developer_test.cpp:437.
    app.shell()->setUser(QString(), Role::Anonymous);

    QVERIFY2(app.window()->findChild<ActionBar *>() != nullptr,
             "the composed shell has no action bar");
    QCOMPARE(enabledActionBarControls(app), 0);

    // Obsolete generation first: it must not be able to bring the shell online.
    injected.emitConnection(kObsoleteGeneration, true);
    injected.emitSnapshot(kObsoleteGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    settle();
    QCOMPARE(enabledActionBarControls(app), 0);

    // Acceptance control: the same events with the current generation do.
    injected.emitConnection(kCurrentGeneration, true);
    injected.emitSnapshot(kCurrentGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    QTRY_VERIFY_WITH_TIMEOUT(enabledActionBarControls(app) > 0, 2000);

    // An accepted current-generation offline event is observable (the shell
    // returns to the offline enabling state).
    injected.emitConnection(kCurrentGeneration, false);
    QTRY_COMPARE_WITH_TIMEOUT(enabledActionBarControls(app), 0, 2000);

    // Obsolete-generation online events must not restore the session.
    injected.emitConnection(kObsoleteGeneration, true);
    settle();
    QCOMPARE(enabledActionBarControls(app), 0);
    injected.emitSnapshot(kObsoleteGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    settle();
    QCOMPARE(enabledActionBarControls(app), 0);

    // Current generation restores the session (acceptance control).
    injected.emitConnection(kCurrentGeneration, true);
    injected.emitSnapshot(kCurrentGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    QTRY_VERIFY_WITH_TIMEOUT(enabledActionBarControls(app) > 0, 2000);

    // Snapshot-content discriminator on the recipe surface: the incompatible
    // snapshot would disable the administrator apply control if it were
    // accepted, so the obsolete case is observable rather than vacuous. The
    // recipe page reads the shell session role (owner correction 2026-09-17:
    // the seam is app.shell()->setUser, not coordinator()->setRole).
    app.shell()->setUser(QStringLiteral("admin"), Role::Admin);
    RecipeWidthPage *recipe = app.window()->findChild<RecipeWidthPage *>();
    QVERIFY2(recipe != nullptr, "the composed shell has no recipe width page");
    QVERIFY2(recipe->applyButton() != nullptr, "the recipe page has no apply control");
    injected.emitSnapshot(kCurrentGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    QTRY_VERIFY_WITH_TIMEOUT(recipe->applyButton()->isEnabled(), 2000);

    injected.emitSnapshot(kObsoleteGeneration, DeviceSnapshot(disabledSnapshot()));
    settle();
    QVERIFY2(recipe->applyButton()->isEnabled(),
             "an obsolete-generation snapshot changed the shell state: the recipe apply "
             "control was disabled by an event from an older generation");

    // The identical content with the current generation must be observable.
    injected.emitSnapshot(kCurrentGeneration, DeviceSnapshot(disabledSnapshot()));
    QTRY_VERIFY_WITH_TIMEOUT(!recipe->applyButton()->isEnabled(), 2000);

    // Recovery with current-generation data (acceptance control).
    injected.emitSnapshot(kCurrentGeneration, DeviceSnapshot(validHomedManualSnapshot()));
    QTRY_VERIFY_WITH_TIMEOUT(recipe->applyButton()->isEnabled(), 2000);

    app.shutdown();
}

// --- OB-6: default production composition --------------------------------------

void PlcHmi007GenerationRejectionTest::defaultCompositionStillCreatesItsOwnGateway()
{
    // The default production composition path must stay unchanged: with
    // plcGateway null and useSimulatedGateway true, Application composes its
    // own simulated gateway and never reuses a caller-owned instance.
    QTemporaryDir injectedDir;
    QVERIFY(injectedDir.isValid());

    EmittingGateway injected;
    AppConfig injectedCfg;
    injectedCfg.plcGateway = &injected;
    injectedCfg.useSimulatedGateway = false;
    injectedCfg.simulatedTickIntervalMs = 0;
    injectedCfg.databasePath = injectedDir.filePath(QStringLiteral("app.db"));
    {
        Application injectedApp(injectedCfg);
        injectedApp.start();
        QVERIFY2(injectedApp.gateway() == &injected,
                 "S-INJECT: the injected gateway must be used");
        injectedApp.shutdown();
    }

    QTemporaryDir defaultDir;
    QVERIFY(defaultDir.isValid());

    AppConfig defaultCfg;
    QVERIFY2(defaultCfg.plcGateway == nullptr,
             "the frozen default of AppConfig::plcGateway must be nullptr");
    defaultCfg.useSimulatedGateway = true;
    defaultCfg.simulatedTickIntervalMs = 0;
    defaultCfg.databasePath = defaultDir.filePath(QStringLiteral("app.db"));

    Application defaultApp(defaultCfg);
    defaultApp.start();
    QVERIFY2(defaultApp.gateway() != nullptr,
             "the default composition path must still compose a gateway");
    QVERIFY2(defaultApp.gateway() != &injected,
             "the default composition path must not reuse the caller-owned injected "
             "instance");

    auto *ownSimulator = qobject_cast<SimulatedPlcGateway *>(defaultApp.gateway());
    QVERIFY2(ownSimulator != nullptr,
             "the default simulated composition must still compose its own "
             "SimulatedPlcGateway");
    for (int i = 0; i < 20 && !ownSimulator->isOnline(); ++i)
        ownSimulator->tick();
    QVERIFY2(ownSimulator->isOnline(),
             "the default composed simulator must still come online");

    defaultApp.shutdown();
}

QTEST_MAIN(PlcHmi007GenerationRejectionTest)
#include "plc_hmi_007_generation_rejection_test.moc"
