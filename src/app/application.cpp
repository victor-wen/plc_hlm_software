#include "app/application.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMessageBox>
#include <QThread>
#include <QTimer>

#include "adapters/modbus/qt_modbus_plc_gateway.h"
#include "adapters/modbus/qt_serial_port_discovery.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#ifdef HLM_ENABLE_VISION
#include "adapters/vision/vision_service.h"
#endif
#include "app/lifecycle_controller.h"
#include "application/control_coordinator.h"
#include "ports/iplc_gateway.h"
#include "ports/iserial_port_discovery.h"
#include "ports/ivision_service.h"
#include "ui/MainWindow.h"
#include "ui/pages/alarm_page.h"
#include "ui/pages/audit_log_page.h"
#include "ui/pages/diagnostics_page.h"
#include "ui/pages/manual_control_page.h"
#include "ui/pages/recipe_width_page.h"
#include "ui/pages/users_settings_page.h"
#include "ui/shell/shell_model.h"

namespace hlm {

namespace {

// Serial config persistence keys (spec §8.1). Stored in the app_settings
// table as plain string values.
constexpr const char *kSerialComPort = "serial.comPort";
constexpr const char *kSerialStation = "serial.station";
constexpr const char *kSerialBaudRate = "serial.baudRate";
constexpr const char *kSerialStopBits = "serial.stopBits";
constexpr const char *kSerialParity = "serial.parity";
constexpr const char *kSerialTimeoutMs = "serial.timeoutMs";
constexpr const char *kSerialReadRetries = "serial.readRetries";

constexpr quint16 kD122 = 122; // 皮带速度
constexpr quint16 kD204 = 204; // 脉冲当量
constexpr quint16 kD220 = 220; // 调宽速度

// Defensive parameter-write confirmation timeout (PLC-HMI-003 D4): a lost
// completion must never leave the settings page pending forever.
constexpr int kParamWriteTimeoutMs = 5000;

// Bounded shutdown window for a pending logout/session-timeout clear
// (PLC-HMI-009 D1(f)). The clear is never reported successful from enqueue
// acceptance, so shutdown must give its correlated write confirmations a
// limited chance to arrive before the gateway stops, then converge honestly.
// The window is wall-clock bounded so shutdown always returns.
constexpr int kShutdownClearWindowMs = 2000;
constexpr int kShutdownClearSliceMs = 10;

// Maps a coordinator terminal result onto the projected lifecycle state. The
// coordinator's existing commandResult(cmd, ok, detail) signature is unchanged
// (independent tests depend on it); the detail it produces is the authoritative
// human-readable outcome and is never discarded.
OperatorCommandState lifecycleStateForResult(bool ok, const QString &detail)
{
    if (ok)
        return OperatorCommandState::Succeeded;
    if (detail.contains(QStringLiteral("超时")))
        return OperatorCommandState::TimedOut;
    if (detail.contains(QStringLiteral("通讯中断")))
        return OperatorCommandState::CommunicationsLost;
    return OperatorCommandState::Failed;
}

} // namespace

Application::Application(const AppConfig &config, QObject *parent)
    : QObject(parent)
    , m_cfg(config)
{
    m_loadedSerialCfg = m_cfg.serial;
    createObjects();
    wireSignals();
}

Application::~Application()
{
    shutdown();

    // These objects intentionally have no QObject parent: the window is a
    // top-level widget, while the services move to worker threads. Destroy
    // them explicitly before QObject tears down the parented ShellModel and
    // other application objects which the window still references.
    delete m_window;
    m_window = nullptr;
    delete m_vision;
    m_vision = nullptr;
    delete m_db;
    m_db = nullptr;
}

void Application::createObjects()
{
    m_shell = new ShellModel(this);
    // The window and the application layer must observe the same model.
    // Previously MainWindow allocated a second ShellModel, so PLC/session
    // updates were delivered to a model that the visible widgets did not use.
    m_window = new MainWindow(nullptr, m_shell);
    m_window->setParent(nullptr); // top-level window

    // Pages are created inside MainWindow; fetch the pointers (spec §11.3).
    m_usersPage = m_window->findChild<UsersSettingsPage *>();
    m_recipePage = m_window->findChild<RecipeWidthPage *>();
    m_manualPage = m_window->findChild<ManualControlPage *>();
    m_alarmPage = m_window->findChild<AlarmPage *>();
    m_auditPage = m_window->findChild<AuditLogPage *>();
    m_diagPage = m_window->findChild<DiagnosticsPage *>();

    // Passive serial discovery (spec §8.1, C-11): the configuration carries
    // only the port pointer; the real adapter is composed here unless an
    // embedding or test injected one. Nothing is enumerated at startup and
    // enumeration never touches the gateway.
    qRegisterMetaType<SerialEnumerationResult>("hlm::SerialEnumerationResult");
    m_discovery = m_cfg.serialPortDiscovery;
    if (!m_discovery)
        m_discovery = new QtSerialPortDiscovery(this);

    // Gateway: real Modbus by default. The in-process simulator is enabled
    // only by the explicit --sim command-line option (spec §14.2). This keeps
    // a missing physical/virtual PLC from being reported as online.
    // An injected gateway (S-INJECT, PLC-HMI-007 D7) takes precedence over
    // both: it is caller-owned, so no composition and no ownership transfer.
    if (m_cfg.plcGateway) {
        m_gw = m_cfg.plcGateway;
    } else if (m_cfg.useSimulatedGateway) {
        m_gw = new SimulatedPlcGateway(this);
    } else {
        m_gw = new QtModbusPlcGateway(
        QtModbusPlcGateway::Config::fromSettings(m_cfg.serial), this);
    }
    // The composition root owns the gateway generation: it increments before
    // every replacement and rejects events from older generations (D4).
    m_gw->setGatewayGeneration(++m_gatewayGeneration);

    // SimulatedPlcGateway itself remains deterministic for unit/integration
    // tests. The composition root supplies the real-time clock only for the
    // interactive --sim application mode.
    m_simulationTimer = new QTimer(this);
    m_simulationTimer->setInterval(qMax(1, m_cfg.simulatedTickIntervalMs));
    connect(m_simulationTimer, &QTimer::timeout, this, [this]() {
        if (auto *sim = qobject_cast<SimulatedPlcGateway *>(m_gw))
            sim->tick();
    });

    // Coordinator: PulseTransport routes into the revised submission port
    // (spec §8.5, PLC-HMI-003 D1/D5). Every callback returns the structured
    // SubmissionResult so the coordinator can correlate completions by
    // request identity + gateway generation.
    //
    // Reset is now a fire-and-confirm-by-fixed-delay command (PLC-HMI-010 D3):
    // its completion is decided by the coordinator clock. The composition root
    // therefore shares the deterministic simulator time base whenever a test
    // owns that clock (simulatedTickIntervalMs == 0), so gw->tick() alone
    // advances a pending reset. Real gateways and the interactive --sim timer
    // keep the wall clock, preserving the real fixed 200 ms.
    std::function<qint64()> coordinatorNowMs;
    if (m_cfg.useSimulatedGateway && m_cfg.simulatedTickIntervalMs == 0) {
        coordinatorNowMs = [this]() -> qint64 {
            if (auto *sim = qobject_cast<SimulatedPlcGateway *>(m_gw))
                return static_cast<qint64>(sim->elapsedSeconds()) * 1000;
            return QDateTime::currentMSecsSinceEpoch();
        };
    }
    ControlCoordinator::PulseTransport transport;
    transport.startPulse = [this](quint16 address) {
        return m_gw->submitPulse(address);
    };
    transport.writeHold = [this](quint16 address, bool value) {
        return m_gw->submitWriteCoil(address, value, CommandPriority::Normal);
    };
    transport.writeCoil = [this](quint16 address, bool value,
                                 CommandPriority priority) {
        return m_gw->submitWriteCoil(address, value, priority);
    };
    transport.writeRegister = [this](quint16 address, quint16 value,
                                     CommandPriority priority) {
        return m_gw->submitWriteRegister(address, value, priority);
    };
    m_coordinator = new ControlCoordinator(
        std::move(transport), ControlCoordinator::Config(),
        std::move(coordinatorNowMs), this);

    // Database: no parent, moved onto its own worker thread by start()
    // (spec §7.3).
    m_db = new DatabaseService(m_cfg.databasePath);

    // Vision: only when the module is compiled in (spec §6, §7.4).
#ifdef HLM_ENABLE_VISION
    m_vision = new VisionService(false, nullptr);
#endif

    // Lifecycle: audit goes to the database, heartbeat stop is handled by
    // the gateway stop (spec §13).
    m_lifecycle = new LifecycleController(
        m_shell, m_coordinator, m_window, m_usersPage,
        [this](const QString &action, const QString &target,
               const QString &redactedParams, AuditResult result,
               const QString &reason) {
            AuditRecord a;
            a.occurredAt = QDateTime::currentDateTimeUtc();
            a.username = m_lifecycle ? m_lifecycle->currentUsername()
                                     : QStringLiteral("anonymous");
            a.role = m_shell ? m_shell->role() : Role::Anonymous;
            a.action = action;
            a.target = target;
            a.redactedParameters = redactedParams;
            a.result = result;
            a.reason = reason;
            m_db->appendAudit(a);
        },
        []() { /* heartbeat stops with the gateway (spec §13) */ }, this);

    // Restricted-mode verdict propagation (PLC-HMI-008 D1/D2): the coordinator
    // consults the single LifecycleController::commandAllowed predicate at its
    // command-entry chokepoint, so page entries that reach the coordinator
    // directly (manual hold/latch/bypass, recipe apply, mode switch) are
    // enforced identically to the Application-routed commands. The callback
    // keeps the verdict authoritative on restricted-mode enter/exit; no second
    // restricted predicate exists. A bare coordinator (unit tests) has no gate
    // and keeps its previous behavior.
    m_coordinator->setCommandGate([this](Command cmd) -> QString {
        if (m_lifecycle != nullptr && !m_lifecycle->commandAllowed(cmd))
            return m_lifecycle->commandRejectionReason();
        return QString();
    });
}

void Application::wireSignals()
{
    wireGateway(m_gw);

    // --- coordinator -> shell / recipe page ----------------------------------
    // Every rejection, pending phase and terminal result is projected into the
    // one OperatorCommandStatus on ShellModel (D6); no reason or detail is
    // discarded here. The legacy pending flag stays for existing consumers.
    connect(m_coordinator, &ControlCoordinator::commandAccepted, this,
            [this](Command cmd) {
                m_shell->setCommandPending(cmd, true);
                publishOperatorStatus(cmd, OperatorCommandState::Accepted,
                                      QStringLiteral("命令已提交, 等待 PLC 确认"),
                                      /*newRequest=*/true);
            });
    connect(m_coordinator, &ControlCoordinator::commandPending, this,
            [this](Command cmd) { m_shell->setCommandPending(cmd, true); });
    connect(m_coordinator, &ControlCoordinator::commandPendingDetail, this,
            [this](Command cmd, const QString &detail) {
                publishOperatorStatus(cmd, OperatorCommandState::Pending, detail,
                                      /*newRequest=*/false);
            });
    connect(m_coordinator, &ControlCoordinator::commandRejected, this,
            [this](Command cmd, const QString &reason) {
                m_shell->setCommandPending(cmd, false);
                publishOperatorStatus(cmd, OperatorCommandState::Rejected, reason,
                                      /*newRequest=*/true);
            });
    connect(m_coordinator, &ControlCoordinator::commandResult, this,
            &Application::handleCommandResult);

    // --- MainWindow -> coordinator / lifecycle --------------------------------
    connect(m_window, &MainWindow::commandRequested, this,
            &Application::onCommandRequested);
    connect(m_window, &MainWindow::modeSwitchRequested, m_coordinator,
            &ControlCoordinator::setMode);
    connect(m_window, &MainWindow::loginLogoutRequested, this,
            &Application::onLoginLogoutRequested);

    // --- UsersSettingsPage -> database / coordinator --------------------------
    connect(m_usersPage, &UsersSettingsPage::createInitialAdminRequested, m_db,
            &DatabaseService::createInitialAdmin);
    connect(m_usersPage, &UsersSettingsPage::loginRequested, m_db,
            &DatabaseService::login);
    connect(m_usersPage, &UsersSettingsPage::logoutRequested, this,
            &Application::handleLogoutRequested);
    connect(m_usersPage, &UsersSettingsPage::logoutClearRequested, m_lifecycle,
            &LifecycleController::onLogoutClearRequested);
    connect(m_usersPage, &UsersSettingsPage::addUserRequested, m_db,
            &DatabaseService::addUser);
    connect(m_usersPage, &UsersSettingsPage::changePasswordRequested, m_db,
            &DatabaseService::changePassword);
    connect(m_usersPage, &UsersSettingsPage::deleteUserRequested, m_db,
            &DatabaseService::deleteUser);
    connect(m_usersPage, &UsersSettingsPage::saveSerialSettingsRequested, this,
            &Application::persistSerialSettings);
    // Explicit administrator action only: the page triggers one passive
    // enumeration; the completion is correlated by request id.
    connect(m_usersPage, &UsersSettingsPage::enumerateSerialPortsRequested, this,
            [this]() {
                if (!m_discovery)
                    return;
                m_pendingEnumerationRequestId =
                    m_discovery->enumerateAvailablePorts();
            });
    connect(m_discovery, &ISerialPortDiscovery::enumerationCompleted, this,
            &Application::handleEnumerationCompleted);
    connect(m_usersPage, &UsersSettingsPage::writeParameterRequested, this,
            &Application::handleParameterWrite);
    connect(m_usersPage, &UsersSettingsPage::d204WriteRequested, this,
            &Application::handleD204Write);

    // --- RecipeWidthPage -> coordinator / database ---------------------------
    connect(m_recipePage, &RecipeWidthPage::applyAdjustRequested, m_coordinator,
            &ControlCoordinator::adjustWidth);
    connect(m_recipePage, &RecipeWidthPage::saveRecipeRequested, this,
            [this](const QString &name, int targetWidthRaw) {
                RecipeRecord r;
                r.name = name;
                r.targetWidthRaw = targetWidthRaw;
                r.createdBy = m_lifecycle ? m_lifecycle->currentUsername()
                                          : QStringLiteral("anonymous");
                r.updatedBy = r.createdBy;
                // Page-local pending before the database round-trip (D2/D7).
                m_recipePage->setRecipeSavePending();
                m_db->saveRecipe(r);
            });
    connect(m_recipePage, &RecipeWidthPage::deleteRecipeRequested, this,
            [this](qint64 recipeId) {
                // Page-local pending before the database round-trip (D3/D7).
                m_recipePage->setRecipeDeletePending();
                m_db->deleteRecipe(recipeId);
            });

    // --- ManualControlPage -> coordinator -------------------------------------
    connect(m_manualPage, &ManualControlPage::manualHoldRequested, m_coordinator,
            &ControlCoordinator::manualHold);
    connect(m_manualPage, &ManualControlPage::manualLatchRequested, m_coordinator,
            &ControlCoordinator::manualLatch);
    connect(m_manualPage, &ManualControlPage::bypassRequested, m_coordinator,
            &ControlCoordinator::bypass);
    // 测试信号 M114-M117 (user decision 2026-09-22): the page sends the address,
    // the coordinator owns the command identity and the pulse.
    connect(m_manualPage, &ManualControlPage::simStationPulseRequested,
            m_coordinator, &ControlCoordinator::simStationPulse);
    // 调宽速度 D220 就地配置 (user decision 2026-09-22): validated by the same
    // single parameter-write path, result reported on the manual page.
    connect(m_manualPage, &ManualControlPage::widthSpeedWriteRequested, this,
            &Application::handleManualWidthSpeedWrite);

    // --- AlarmPage / AuditLogPage -> database ---------------------------------
    connect(m_alarmPage, &AlarmPage::requestReload, this, [this]() {
        m_alarmPage->setLoading();
        m_db->listRecentAlarms(200);
    });
    connect(m_auditPage, &AuditLogPage::requestReload, this, [this]() {
        m_auditPage->setLoading();
        m_auditLoadedCount = 0;
        m_db->listRecentAudit(200);
    });
    connect(m_auditPage, &AuditLogPage::requestMore, this, [this]() {
        // 滚动加载: fetch the next page beyond what is already loaded
        // (spec §12); listRecentAudit(200, offset) pages by offset.
        m_db->listRecentAudit(200, m_auditLoadedCount);
    });

    // --- DatabaseService -> pages / lifecycle ---------------------------------
    connect(m_db, &DatabaseService::ready, this, &Application::onReady);
    connect(m_db, &DatabaseService::databaseRestricted, this,
            [this](const QString &reason) {
                m_currentUserId = -1;
                if (m_d204Pending)
                    m_d204Cancelled = true;
                m_lifecycle->enterRestrictedMode(reason);
                // Persisted settings are unavailable in restricted mode. Start
                // the configured fallback gateway so Stop/Estop remain usable.
                startGatewayIfNeeded();
                LoginResult unavailable;
                unavailable.ok = false;
                unavailable.reason = QStringLiteral("database restricted");
                m_usersPage->setLoginResult(unavailable);
                m_window->setCurrentPage(6);
            });
    connect(m_db, &DatabaseService::initialAdminNeeded, this,
            [this](bool needed) {
                m_usersPage->setNeedsInitialAdmin(needed);
                // First launch must be self-explanatory: take the operator
                // directly to the mandatory bootstrap card instead of leaving
                // it hidden behind the last navigation item.
                if (needed)
                    m_window->setCurrentPage(6);
            });
    connect(m_db, &DatabaseService::initialAdminCreated, this,
            [this](bool ok, const QString &error) {
                m_usersPage->setInitialAdminResult(ok, error);
                if (ok) {
                    m_usersPage->setNeedsInitialAdmin(false);
                    m_db->listUsers();
                }
            });
    connect(m_db, &DatabaseService::loginResult, this,
            &Application::handleLoginResult);
    connect(m_db, &DatabaseService::usersLoaded, m_usersPage,
            &UsersSettingsPage::setUsers);
    connect(m_db, &DatabaseService::userAdded, this,
            [this](bool ok, const QString &error) {
                m_usersPage->setAddUserResult(ok, error);
                if (ok)
                    m_db->listUsers();
            });
    connect(m_db, &DatabaseService::userDeleted, this,
            [this](bool ok, const QString &error) {
                m_usersPage->setDeleteUserResult(ok, error);
                if (ok)
                    m_db->listUsers();
            });
    connect(m_db, &DatabaseService::passwordChanged, m_usersPage,
            &UsersSettingsPage::setPasswordChangeResult);
    connect(m_db, &DatabaseService::recipesLoaded, m_recipePage,
            &RecipeWidthPage::setRecipes);
    connect(m_db, &DatabaseService::recipeSaved, this,
            [this](bool ok, const QString &error) {
                // Route the database outcome into the page-local status; the
                // page preserves it across the follow-up reload (D2).
                m_recipePage->setRecipeSaveResult(ok, error);
                if (ok)
                    m_db->listRecipes();
            });
    connect(m_db, &DatabaseService::recipeDeleted, this,
            [this](bool ok, const QString &error) {
                // Route the database outcome into the page-local status; the
                // confirmed deletion reload then clears the selection/editors
                // in RecipeWidthPage::setRecipes (D1, D3).
                m_recipePage->setRecipeDeleteResult(ok, error);
                if (ok)
                    m_db->listRecipes();
            });
    connect(m_db, &DatabaseService::settingLoaded, this,
            &Application::handleSettingLoaded);
    connect(m_db, &DatabaseService::serialSettingsBatchSaved, this,
            &Application::handleSerialSettingsBatchSaved);
    connect(m_db, &DatabaseService::passwordVerified, this,
            &Application::handlePasswordVerified);
    connect(m_db, &DatabaseService::recentAlarmsLoaded, this,
            &Application::handleAlarmsLoaded);
    connect(m_db, &DatabaseService::recentAuditLoaded, this,
            &Application::handleAuditLoaded);

    // --- VisionService -> diagnostics page ------------------------------------
    if (m_vision) {
        connect(m_vision, &IVisionService::selfTestPassed, m_diagPage,
                [this](const QString &version) {
                    m_diagPage->setVisionStatus(version, true, QString());
                });
        connect(m_vision, &IVisionService::selfTestFailed, m_diagPage,
                [this](const QString &reason) {
                    m_diagPage->setVisionStatus(QString(), false, reason);
                });
    } else {
        m_diagPage->setVisionStatus(QString(), false,
                                    QStringLiteral("视觉模块未启用"));
    }
}

// True when an event's gateway generation is not older than the generation
// the composition root assigned to the current gateway (D4: reject stale
// events from a replaced gateway).
bool Application::acceptGatewayGeneration(quint64 generation) const
{
    return generation >= m_gatewayGeneration;
}

// Wires every gateway signal. Called for the initial gateway and again after a
// serial-config rebuild (spec §8.1). Every event is generation-checked and
// routed; no qobject_cast on the concrete gateway type is needed (D4).
void Application::wireGateway(IPlcGateway *gw)
{
    connect(gw, &IPlcGateway::snapshotReady, this,
            [this](quint64 generation, const DeviceSnapshot &s) {
                if (!acceptGatewayGeneration(generation))
                    return; // obsolete gateway generation: rejected
                m_coordinator->onSnapshot(s);
                m_shell->updateSnapshot(s);
                m_db->feedPlcAlarmSnapshot(s.faultCode(), s.m14(), s.m4(),
                                           s.sequence());
            });
    connect(gw, &IPlcGateway::connectionStateChanged, this,
            [this](quint64 generation, bool online) {
                if (!acceptGatewayGeneration(generation))
                    return; // obsolete gateway generation: rejected
                m_coordinator->onConnectionChanged(online);
                m_shell->setOnline(online);
                if (!online) {
                    // Never leave a parameter write pending across a link
                    // loss or gateway stop (D4).
                    failAllPendingParamWrites(QStringLiteral("通讯中断"));
                }
            });
    connect(gw, &IPlcGateway::submissionCompleted, this,
            &Application::handleSubmissionCompleted);
    // The coordinator correlates completions by request identity + generation
    // and converges failed writes (spec §10.3, D5).
    connect(gw, &IPlcGateway::submissionCompleted, m_coordinator,
            &ControlCoordinator::onSubmissionCompleted);
    // Communication statistics come from the port signal itself (spec §16,
    // D4): no concrete-gateway cast, and the real per-block age is displayed.
    connect(gw, &IPlcGateway::commStatsChanged, this,
            [this](const PlcCommStats &stats) {
                if (!acceptGatewayGeneration(stats.gateway_generation))
                    return;
                CommStats display;
                display.lastDataAgeMs = stats.per_block_age_ms;
                display.sequence = stats.snapshot_sequence;
                display.reconnectCount = int(stats.reconnect_count);
                display.failedPolls = int(stats.failed_polls);
                m_diagPage->setCommStats(display);
            });
}

void Application::startGatewayIfNeeded()
{
    if (!m_gw || m_gatewayStarted || m_shutdownDone)
        return;
    m_gw->start();
    m_gatewayStarted = true;
    if (m_cfg.useSimulatedGateway && m_cfg.simulatedTickIntervalMs > 0
        && !m_simulationTimer->isActive())
        m_simulationTimer->start();
}

void Application::start()
{
    // The simulator has no persisted transport settings and can start
    // immediately. The real gateway starts only after all serial settings have
    // been loaded, so it never opens the default COM port by mistake.
    m_db->start();
    if (m_cfg.useSimulatedGateway)
        startGatewayIfNeeded();
    if (m_vision)
        m_vision->start();
    m_lifecycle->startSessionTimer();
    m_window->show();
}

void Application::shutdown()
{
    // Idempotent: aboutToQuit and the destructor both call this; the second
    // call must not re-issue M42/M106-M111 clears against a stopped gateway
    // (Task 20 review).
    if (m_shutdownDone)
        return;
    m_shutdownDone = true;
    if (m_simulationTimer)
        m_simulationTimer->stop();
    // Ordered shutdown (spec §13): clear M42/M106-M111 + stop heartbeat, then
    // gateway, database, vision. M100 is never auto-cleared.
    if (m_lifecycle)
        m_lifecycle->shutdown();
    // PLC-HMI-009 D1(f): the clear is never reported successful from enqueue
    // acceptance, so give its correlated write confirmations a bounded chance
    // to arrive before the gateway stops. The window is wall-clock bounded and
    // always returns; an in-process simulator is ticked like the interactive
    // simulation timer, but only when the composition root itself drives the
    // simulator (tests with simulatedTickIntervalMs=0 own the clock and keep
    // their deterministic tick semantics). Any clear still pending afterwards
    // is converged honestly (failure) before the gateway stops.
    if (m_coordinator && m_coordinator->logoutClearPending()) {
        const bool drivenSimulator =
            m_cfg.useSimulatedGateway && m_cfg.simulatedTickIntervalMs > 0;
        QElapsedTimer window;
        window.start();
        while (m_coordinator->logoutClearPending()
               && window.elapsed() < kShutdownClearWindowMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents,
                                            kShutdownClearSliceMs);
            if (drivenSimulator) {
                if (auto *sim = qobject_cast<SimulatedPlcGateway *>(m_gw))
                    sim->tick();
            }
        }
        if (m_coordinator->logoutClearPending()) {
            m_coordinator->failPendingLogoutClear(
                QStringLiteral("注销清零: 通讯中断, 连续输出清零未确认"));
        }
    }
    if (m_gw)
        m_gw->stop();
    m_gatewayStarted = false;
    if (m_db)
        m_db->stop();
    if (m_vision)
        m_vision->stop();
}

// --- database ready: initial bootstrap ---------------------------------------

void Application::onReady()
{
    m_db->needsInitialAdmin();
    m_db->listUsers();
    m_db->listRecipes();
    m_db->runRetentionCleanup();
    // Load the persisted serial config for echo (spec §8.1).
    m_pendingSerialLoads = 7;
    m_db->getSetting(QString::fromLatin1(kSerialComPort));
    m_db->getSetting(QString::fromLatin1(kSerialStation));
    m_db->getSetting(QString::fromLatin1(kSerialBaudRate));
    m_db->getSetting(QString::fromLatin1(kSerialStopBits));
    m_db->getSetting(QString::fromLatin1(kSerialParity));
    m_db->getSetting(QString::fromLatin1(kSerialTimeoutMs));
    m_db->getSetting(QString::fromLatin1(kSerialReadRetries));
}

// --- serial config persistence (spec §8.1, NF-08) ----------------------------

void Application::persistSerialSettings(const SerialConnectionSettings &settings)
{
    if (m_pendingSerialBatchId != 0) {
        // Immediate visible rejection: the accepted request owns the batch and
        // the duplicate is never queued or silently dropped (spec NF-08).
        m_usersPage->setSerialSettingsSaveResult(
            false, QStringLiteral("已有保存请求正在处理中，本次请求未提交"));
        return;
    }

    SettingsBatch batch;
    batch.batch_id = ++m_nextSerialBatchId;
    batch.settings = settings;
    // The audit column is NOT NULL: an absent session must never bind a null
    // string, so the acting user falls back to "anonymous" like the audit path.
    const QString username =
        m_lifecycle ? m_lifecycle->currentUsername() : QString();
    batch.updated_by = username.isEmpty() ? QStringLiteral("anonymous") : username;
    m_pendingSerialBatchId = batch.batch_id;
    m_pendingSerialSettings = settings;
    m_usersPage->setSerialSettingsSavePending();
    m_db->saveSerialSettingsBatch(batch);
}

void Application::handleSerialSettingsBatchSaved(const SettingsBatchResult &result)
{
    // Correlate strictly by batch id: a result for an unknown or obsolete
    // request must not complete the outstanding save or touch the gateway.
    if (result.batch_id == 0 || result.batch_id != m_pendingSerialBatchId)
        return;
    m_pendingSerialBatchId = 0;

    if (!result.committed) {
        // Failure: the active gateway and the previously committed settings
        // stay unchanged; only the page shows the terminal failure (NF-08).
        m_usersPage->setSerialSettingsSaveResult(false, result.error);
        return;
    }

    const SerialConnectionSettings committed = m_pendingSerialSettings;
    m_usersPage->setSerialSettings(committed);
    m_usersPage->setSerialSettingsSaveResult(true, QString());

    // The gateway rebuild begins only here, after the matching successful
    // batch result. Serial transport settings only affect the real Modbus
    // gateway: the in-process simulator has no serial transport and must not
    // be stopped or replaced, and an injected gateway (S-INJECT, PLC-HMI-007
    // D7) is caller-owned and stays in place.
    if (!m_cfg.useSimulatedGateway && m_cfg.plcGateway == nullptr)
        rebuildGateway(committed);
}

void Application::handleEnumerationCompleted(const SerialEnumerationResult &result)
{
    // Only the outstanding request may complete; an unrelated/obsolete id is
    // ignored and cannot update the page or the gateway (spec C-11).
    if (m_pendingEnumerationRequestId == 0
        || result.enumeration_request_id != m_pendingEnumerationRequestId) {
        return;
    }
    m_pendingEnumerationRequestId = 0;
    if (!result.completion_error.isEmpty()) {
        m_usersPage->setSerialSettingsEnumerationError(result.completion_error);
        return;
    }
    m_usersPage->setDiscoveredSerialPorts(result.discovered_descriptors);
}

void Application::handleSettingLoaded(const std::optional<SettingRecord> &setting)
{
    if (m_pendingSerialLoads <= 0)
        return;
    // Count every load (including a missing key) so first run uses defaults.
    --m_pendingSerialLoads;
    if (setting) {
        const QString &key = setting->key;
        const QString &value = setting->typedValue;
        bool ok = false;
        if (key == QString::fromLatin1(kSerialComPort)) {
            m_loadedSerialCfg.port_name = value;
        } else if (key == QString::fromLatin1(kSerialStation)) {
            const int v = value.toInt(&ok);
            if (ok)
                m_loadedSerialCfg.station = v;
        } else if (key == QString::fromLatin1(kSerialBaudRate)) {
            const int v = value.toInt(&ok);
            if (ok)
                m_loadedSerialCfg.baud_rate = v;
        } else if (key == QString::fromLatin1(kSerialStopBits)) {
            const int v = value.toInt(&ok);
            if (ok)
                m_loadedSerialCfg.stop_bits = v;
        } else if (key == QString::fromLatin1(kSerialParity)) {
            m_loadedSerialCfg.parity = value;
        } else if (key == QString::fromLatin1(kSerialTimeoutMs)) {
            const int v = value.toInt(&ok);
            if (ok)
                m_loadedSerialCfg.timeout_ms = v;
        } else if (key == QString::fromLatin1(kSerialReadRetries)) {
            const int v = value.toInt(&ok);
            if (ok)
                m_loadedSerialCfg.read_retries = v;
        }
    }

    if (m_pendingSerialLoads > 0)
        return;

    m_pendingSerialLoads = 0;
    m_usersPage->setSerialSettings(m_loadedSerialCfg);
    // An injected gateway (S-INJECT, PLC-HMI-007 D7) is caller-owned: the
    // persisted serial settings must not replace it.
    if (!m_cfg.useSimulatedGateway && m_cfg.plcGateway == nullptr)
        rebuildGateway(m_loadedSerialCfg);
}

// Rebuilds the gateway with a new serial configuration: stop the old one,
// create the new one, re-wire every signal, start (spec §8.1).
void Application::rebuildGateway(const SerialConnectionSettings &cfg)
{
    if (m_gw) {
        // D4: disconnect every old-gateway signal before deletion, converge
        // pending commands/parameter writes, and only then replace.
        m_gw->stop();
        disconnect(m_gw, nullptr, nullptr, nullptr);
        m_coordinator->onConnectionChanged(false);
        failAllPendingParamWrites(QStringLiteral("串口配置已更换, 参数写入未确认"));
        m_gatewayStarted = false;
        m_gw->deleteLater();
        m_gw = nullptr;
    }
    // Increment the generation before the new gateway exists so any late
    // event from the old one is rejected.
    ++m_gatewayGeneration;
    if (m_cfg.useSimulatedGateway) {
        m_gw = new SimulatedPlcGateway(this);
    } else {
        m_gw = new QtModbusPlcGateway(
            QtModbusPlcGateway::Config::fromSettings(cfg), this);
    }
    m_gw->setGatewayGeneration(m_gatewayGeneration);
    wireGateway(m_gw);
    startGatewayIfNeeded();
}

// --- parameter writes (D122/D220/D204, spec §11.3) ---------------------------

bool Application::validateParameterWrite(quint16 address, quint16 value,
                                         QString *error) const
{
    auto reject = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    if (m_shell->role() != Role::Admin)
        return reject(QStringLiteral("仅管理员可修改设备参数"));

    if (address == kD122) {
        return value >= 100 && value <= 20000
            ? true
            : reject(QStringLiteral("D122 皮带速度需在 100-20000 Hz 之间"));
    }

    if (address != kD204 && address != kD220)
        return reject(QStringLiteral("不支持的参数地址"));

    // Each parameter is validated independently against its own decoded PLC
    // range (PLC-HMI-005 D4): the obsolete D204*D220 frequency product and the
    // confirmed-counterpart coupling are gone; PLC data freshness no longer
    // gates a write because no cross-field combination is validated.
    if (address == kD204) {
        return value >= 1 && value <= 32767
            ? true
            : reject(QStringLiteral("D204 脉冲当量需在 1-32767 之间"));
    }

    return value >= 1 && value <= 15
        ? true
        : reject(QStringLiteral("D220 调宽速度需在 1-15 mm/s 之间"));
}

void Application::handleParameterWrite(quint16 address, quint16 value)
{
    // Parameter writes are user-initiated commands: restricted mode blocks
    // them through the same single verdict and shows the deterministic reason
    // (PLC-HMI-008 D1).
    if (m_lifecycle != nullptr
        && !m_lifecycle->commandAllowed(Command::ParameterChange)) {
        m_usersPage->setParameterWriteResult(false,
                                             m_lifecycle->commandRejectionReason());
        return;
    }
    QString error;
    if (!validateParameterWrite(address, value, &error)) {
        m_usersPage->setParameterWriteResult(false, error);
        return;
    }
    submitParameterWrite(address, value);
}

void Application::handleD204Write(quint16 value, const QString &adminPassword)
{
    // Same restricted-mode gate as every other user-initiated parameter write:
    // no password round-trip and no register write while restricted.
    if (m_lifecycle != nullptr
        && !m_lifecycle->commandAllowed(Command::ParameterChange)) {
        m_usersPage->setParameterWriteResult(false,
                                             m_lifecycle->commandRejectionReason());
        return;
    }
    if (m_d204Pending) {
        m_usersPage->setParameterWriteResult(
            false, QStringLiteral("已有 D204 密码验证正在进行"));
        return;
    }

    QString error;
    if (!validateParameterWrite(kD204, value, &error)) {
        m_usersPage->setParameterWriteResult(false, error);
        return;
    }

    m_d204Pending = true;
    m_d204Cancelled = false;
    m_d204PendingUserId = m_currentUserId;
    m_d204Value = value;
    m_db->verifyPassword(m_d204PendingUserId, adminPassword);
}

void Application::handlePasswordVerified(bool ok)
{
    if (!m_d204Pending)
        return;

    const bool cancelled = m_d204Cancelled;
    const qint64 verifiedUserId = m_d204PendingUserId;
    const quint16 value = m_d204Value;
    m_d204Pending = false;
    m_d204Cancelled = false;
    m_d204PendingUserId = -1;

    if (cancelled || m_shell->role() != Role::Admin
        || m_currentUserId != verifiedUserId) {
        m_usersPage->setParameterWriteResult(
            false, QStringLiteral("会话已变化，D204 写入已取消"));
        return;
    }
    if (!ok) {
        m_usersPage->setParameterWriteResult(false,
                                             QStringLiteral("管理员密码验证失败"));
        return;
    }

    QString error;
    if (!validateParameterWrite(kD204, value, &error)) {
        m_usersPage->setParameterWriteResult(false, error);
        return;
    }

    submitParameterWrite(kD204, value);
}

void Application::submitParameterWrite(quint16 address, quint16 value,
                                       ParamWriteSink sink)
{
    const SubmissionResult result =
        m_gw->submitWriteRegister(address, value, CommandPriority::Normal);
    if (!result.accepted) {
        // Rejected synchronously: the page sees the immediate reason, never
        // silence (spec §11.2: 无乐观状态).
        const QString reason = result.immediate_rejection_reason.isEmpty()
            ? QStringLiteral("参数写入被拒绝")
            : result.immediate_rejection_reason;
        if (sink == ParamWriteSink::Manual)
            m_manualPage->setWidthSpeedWriteResult(false, reason);
        else
            m_usersPage->setParameterWriteResult(false, reason);
        return;
    }
    PendingParamWrite pending;
    pending.request_id = result.request_id;
    pending.gateway_generation = result.gateway_generation;
    pending.address = address;
    pending.value = value;
    pending.sink = sink;
    pending.deadline_ms =
        QDateTime::currentMSecsSinceEpoch() + kParamWriteTimeoutMs;
    m_pendingParamWrites.append(pending);

    // Defensive timeout for the parameter write (PLC-HMI-003 D4).
    const quint64 requestId = result.request_id;
    const quint64 generation = result.gateway_generation;
    QTimer::singleShot(kParamWriteTimeoutMs, this,
                       [this, requestId, generation]() {
                           for (int i = 0; i < m_pendingParamWrites.size(); ++i) {
                               const PendingParamWrite p = m_pendingParamWrites.at(i);
                               if (p.request_id == requestId
                                   && p.gateway_generation == generation) {
                                   m_pendingParamWrites.removeAt(i);
                                   reportParamWriteResult(
                                       p, false,
                                       QStringLiteral("参数写入确认超时"));
                                   return;
                               }
                           }
                       });
}

void Application::reportParamWriteResult(const PendingParamWrite &pending,
                                         bool ok, const QString &detail)
{
    if (pending.sink == ParamWriteSink::Manual) {
        // The manual page's only parameter is D220 调宽速度: name the confirmed
        // value in the success detail so the operator sees what was written.
        m_manualPage->setWidthSpeedWriteResult(
            ok, ok ? QStringLiteral("调宽速度已写入 %1 mm/s").arg(pending.value)
                   : detail);
        return;
    }
    m_usersPage->setParameterWriteResult(ok, detail);
}

void Application::handleManualWidthSpeedWrite(quint16 value)
{
    // Same single restricted-mode verdict as every other user-initiated
    // parameter write (PLC-HMI-008 D1), but the outcome goes back to the page
    // the operator clicked on.
    if (m_lifecycle != nullptr
        && !m_lifecycle->commandAllowed(Command::ParameterChange)) {
        m_manualPage->setWidthSpeedWriteResult(
            false, m_lifecycle->commandRejectionReason());
        return;
    }
    QString error;
    if (!validateParameterWrite(kD220, value, &error)) {
        m_manualPage->setWidthSpeedWriteResult(false, error);
        return;
    }
    submitParameterWrite(kD220, value, ParamWriteSink::Manual);
}

void Application::handleSubmissionCompleted(const SubmissionCompletion &completion)
{
    if (!acceptGatewayGeneration(completion.gateway_generation))
        return; // obsolete generation: rejected
    // Correlate by request identity + generation only (contract invariant).
    for (int i = 0; i < m_pendingParamWrites.size(); ++i) {
        const PendingParamWrite p = m_pendingParamWrites.at(i);
        if (p.request_id != completion.request_id
            || p.gateway_generation != completion.gateway_generation) {
            continue;
        }
        const bool isParamWrite = completion.operation == PlcOperation::WriteRegister;
        m_pendingParamWrites.removeAt(i);
        if (isParamWrite)
            reportParamWriteResult(p, completion.result, completion.error);
        return;
    }
}

void Application::failAllPendingParamWrites(const QString &reason)
{
    const QVector<PendingParamWrite> pending = m_pendingParamWrites;
    m_pendingParamWrites.clear();
    for (const PendingParamWrite &p : pending)
        reportParamWriteResult(p, false, reason);
}

// --- login / logout ----------------------------------------------------------

void Application::handleLoginResult(const LoginResult &result)
{
    m_usersPage->setLoginResult(result);
    if (result.ok && result.user.has_value()) {
        m_currentUserId = result.user->id;
        m_lifecycle->onLoginSucceeded(*result.user);
    }
}

void Application::handleLogoutRequested()
{
    m_currentUserId = -1;
    if (m_d204Pending)
        m_d204Cancelled = true;
    m_lifecycle->onLogoutRequested();
}

void Application::onLoginLogoutRequested()
{
    if (m_shell->role() != Role::Anonymous) {
        handleLogoutRequested();
    } else {
        // 未登录: 切到用户与设置页 (index 6) 显示登录面板.
        m_window->setCurrentPage(6);
    }
}

// --- command routing + EstopRelease interception (spec §10.6) ----------------

void Application::onCommandRequested(Command cmd)
{
    // Restricted-mode gate (PLC-HMI-008 D1/D2): consult the single
    // LifecycleController verdict before any dispatch. A blocked request is
    // projected immediately as a visible rejection with the deterministic
    // reason and never reaches the coordinator or the PLC.
    if (m_lifecycle != nullptr && !m_lifecycle->commandAllowed(cmd)) {
        publishOperatorStatus(cmd, OperatorCommandState::Rejected,
                              m_lifecycle->commandRejectionReason(),
                              /*newRequest=*/true);
        return;
    }
    if (cmd == Command::EstopSet) {
        // 已处于软件急停且当前用户是管理员: 确认后解除急停; 否则置急停.
        // 非管理员点击解除会被 coordinator 的权限门控拒绝 (spec §11.4).
        if (m_shell->isEstop() && m_shell->role() == Role::Admin) {
            // In this state the control maps to EstopRelease, a different
            // Command verdict: restricted mode must block the release and show
            // the same deterministic reason instead of opening the dialog.
            if (m_lifecycle != nullptr
                && !m_lifecycle->commandAllowed(Command::EstopRelease)) {
                publishOperatorStatus(Command::EstopRelease,
                                      OperatorCommandState::Rejected,
                                      m_lifecycle->commandRejectionReason(),
                                      /*newRequest=*/true);
                return;
            }
            const auto answer = QMessageBox::question(
                m_window, QStringLiteral("解除软件急停"),
                QStringLiteral("确认解除软件急停？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes)
                m_coordinator->estopRelease();
            return;
        }
        m_coordinator->estopSet();
        return;
    }
    switch (cmd) {
    case Command::Reset:
        m_coordinator->reset();
        break;
    case Command::HomeStart:
        m_coordinator->homeStart();
        break;
    case Command::Start:
        m_coordinator->start();
        break;
    case Command::Stop:
        m_coordinator->stop();
        break;
    default:
        // An unhandled command must never be a silent no-op (D11/ARCH-016):
        // emit a Qt warning and a visible rejected diagnostic with detail.
        {
            const QString detail = QStringLiteral("未知命令 (代码 %1), 未执行任何操作")
                                       .arg(int(cmd));
            qWarning("PLC-HMI-001: unhandled operator command %d rejected", int(cmd));
            publishOperatorStatus(cmd, OperatorCommandState::Rejected, detail,
                                  /*newRequest=*/true);
        }
        break;
    }
}

void Application::publishOperatorStatus(Command cmd, OperatorCommandState state,
                                        const QString &detail, bool newRequest)
{
    if (newRequest)
        ++m_commandGeneration;
    OperatorCommandStatus status;
    status.command = cmd;
    status.lifecycle_state = state;
    status.human_readable_detail = detail;
    status.command_generation = m_commandGeneration;
    // request_id/gateway_generation stay empty/0 until PLC-HMI-003 wires the
    // real request-identity/gateway-generation correlation.
    m_shell->setOperatorCommandStatus(status);
}

void Application::handleCommandResult(Command cmd, bool ok, const QString &detail)
{
    m_shell->setCommandPending(cmd, false);
    // LogoutClear is an internal clear terminal, not the user request that
    // triggered it: it must own its generation so a cancelled manual
    // confirmation and the clear are two distinct lifecycles instead of two
    // terminals for one generation (PLC-HMI-007 D6-l). Every other command
    // keeps the generation of its own accepted request.
    publishOperatorStatus(cmd, lifecycleStateForResult(ok, detail), detail,
                          /*newRequest=*/cmd == Command::LogoutClear);
    if (cmd == Command::AdjustWidth)
        m_recipePage->setAdjustResult(ok, detail);
}

// --- alarm / audit feeds -----------------------------------------------------

void Application::handleAlarmsLoaded(const QVector<AlarmEventRecord> &alarms)
{
    m_alarmPage->setAlarms(alarms);
}

void Application::handleAuditLoaded(const QVector<AuditRecord> &records)
{
    // 滚动加载: 首次加载替换全部, 后续请求 (offset 分页) 追加整页 (spec §12).
    if (m_auditLoadedCount == 0) {
        m_auditPage->setRecords(records);
    } else {
        m_auditPage->appendRecords(records);
    }
    m_auditLoadedCount += records.size();
}

} // namespace hlm
