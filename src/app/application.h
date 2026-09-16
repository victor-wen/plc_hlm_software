#pragma once

// Application composition root (spec §7, §13, §15.4). Owns every object of
// the running HMI and wires them together. Everything lives on the UI main
// thread except the SQLite worker (DatabaseService) and the vision worker
// (VisionService), which manage their own threads (spec §7.1, §7.3, §7.4).
//
// Responsibilities:
//  - Assemble ShellModel, MainWindow (pages created inside), the PLC gateway
//    (QtModbusPlcGateway by default, SimulatedPlcGateway with --sim),
//    ControlCoordinator, DatabaseService, VisionService (when enabled) and
//    LifecycleController.
//  - Wire every signal: gateway -> coordinator/shell/database/diagnostics,
//    coordinator -> shell/recipe page, MainWindow -> coordinator, pages ->
//    database/coordinator, database -> pages/lifecycle.
//  - EstopRelease interception (spec §10.6): an admin confirming release of a
//    latched software estop goes through a QMessageBox before
//    coordinator.estopRelease(); everything else is coordinator.estopSet().
//  - Serial config persistence (spec §8.1, NF-08): one atomic batch writes all
//    seven serial keys, the matching successful result rebuilds the real
//    gateway, and a duplicate in-flight save is rejected visibly.
//  - Passive serial discovery (spec C-11): enumeration runs only on explicit
//    administrator action and never touches the active gateway.
//  - D204 write (spec §11.3): the admin password is re-verified via
//    DatabaseService::verifyPassword before the register write is issued.
//  - Startup order (spec §13): db.start() -> load persisted serial settings ->
//    gw.start() -> vision.start() -> lifecycle.startSessionTimer() ->
//    window.show(). Restricted DB mode starts the configured fallback gateway.
//  - Shutdown (spec §13): lifecycle.shutdown() (clears M42/M106-M111, stops
//    heartbeat) -> gw.stop() -> db.stop() -> vision.stop(). M100 is never
//    auto-cleared.

#include <QObject>
#include <QString>
#include <QVector>

#include <optional>

#include "app/configuration.h"
#include "application/control_coordinator.h"
#include "domain/operator_command_status.h"
#include "ports/iserial_port_discovery.h"
#include "ports/repositories.h" // SettingRecord, UserRecord, SettingsBatchResult

class QMainWindow;
class QTimer;

namespace hlm {

class ShellModel;
class MainWindow;
class IPlcGateway;
class ControlCoordinator;
class DatabaseService;
class IVisionService;
class LifecycleController;
class UsersSettingsPage;
class RecipeWidthPage;
class ManualControlPage;
class AlarmPage;
class AuditLogPage;
class DiagnosticsPage;

class Application : public QObject
{
    Q_OBJECT

public:
    explicit Application(const AppConfig &config, QObject *parent = nullptr);
    ~Application() override;

    // Starts the database, gateway, vision and session timer, then shows the
    // main window (spec §13 startup order).
    void start();
    // Ordered shutdown (spec §13): lifecycle -> gateway -> database -> vision.
    void shutdown();

    // --- inspection API (integration tests, Task 20d) ------------------------
    MainWindow *window() const { return m_window; }
    ShellModel *shell() const { return m_shell; }
    IPlcGateway *gateway() const { return m_gw; }
    ControlCoordinator *coordinator() const { return m_coordinator; }
    DatabaseService *database() const { return m_db; }
    LifecycleController *lifecycle() const { return m_lifecycle; }
    bool visionEnabled() const { return m_vision != nullptr; }

private:
    void createObjects();
    void wireSignals();
    void wireGateway(IPlcGateway *gw);
    void startGatewayIfNeeded();
    void rebuildGateway(const SerialConnectionSettings &cfg);
    void persistSerialSettings(const SerialConnectionSettings &settings);
    void handleSerialSettingsBatchSaved(const SettingsBatchResult &result);
    void handleEnumerationCompleted(const SerialEnumerationResult &result);
    void handleSettingLoaded(const std::optional<SettingRecord> &setting);
    void handleSubmissionCompleted(const SubmissionCompletion &completion);
    void handleParameterWrite(quint16 address, quint16 value);
    bool validateParameterWrite(quint16 address, quint16 value,
                                QString *error) const;
    void handleD204Write(quint16 value, const QString &adminPassword);
    void handlePasswordVerified(bool ok);
    void handleLogoutRequested();
    void handleLoginResult(const LoginResult &result);
    void handleAuditLoaded(const QVector<AuditRecord> &records);
    void handleAlarmsLoaded(const QVector<AlarmEventRecord> &alarms);
    void handleCommandResult(Command cmd, bool ok, const QString &detail);
    // Projects one operator-command lifecycle state into ShellModel. A new user
    // request (accepted/rejected) increments command_generation; pending and
    // terminal updates keep the current generation (PLC-HMI-001 D6).
    void publishOperatorStatus(Command cmd, OperatorCommandState state,
                               const QString &detail, bool newRequest);
    void onCommandRequested(Command cmd);
    void onLoginLogoutRequested();
    void onReady();

    AppConfig m_cfg;

    ShellModel *m_shell = nullptr;
    MainWindow *m_window = nullptr;
    IPlcGateway *m_gw = nullptr;
    ControlCoordinator *m_coordinator = nullptr;
    DatabaseService *m_db = nullptr;
    IVisionService *m_vision = nullptr; // null when HLM_ENABLE_VISION is off
    LifecycleController *m_lifecycle = nullptr;
    QTimer *m_simulationTimer = nullptr;
    bool m_gatewayStarted = false;

    // Page pointers (created inside MainWindow; fetched via findChild).
    UsersSettingsPage *m_usersPage = nullptr;
    RecipeWidthPage *m_recipePage = nullptr;
    ManualControlPage *m_manualPage = nullptr;
    AlarmPage *m_alarmPage = nullptr;
    AuditLogPage *m_auditPage = nullptr;
    DiagnosticsPage *m_diagPage = nullptr;

    // Current session user id (for D204 re-verification, spec §11.3).
    qint64 m_currentUserId = -1;

    // Monotonic operator-command generation (D6): incremented once per user
    // request, carried by every projected OperatorCommandStatus.
    quint64 m_commandGeneration = 0;

    // Serial config persistence bookkeeping (spec §8.1, NF-08): one atomic
    // batch in flight at a time, correlated by batch id.
    quint64 m_nextSerialBatchId = 0;
    quint64 m_pendingSerialBatchId = 0;
    SerialConnectionSettings m_pendingSerialSettings;
    int m_pendingSerialLoads = 0;
    SerialConnectionSettings m_loadedSerialCfg;

    // Passive discovery boundary: injected or owned (created in createObjects).
    ISerialPortDiscovery *m_discovery = nullptr;
    quint64 m_pendingEnumerationRequestId = 0;

    // Idempotency guard: shutdown() runs from aboutToQuit and again from the
    // destructor; the second call must not re-issue clears against a stopped
    // gateway (Task 20 review).
    bool m_shutdownDone = false;

    // Parameter writes (D122/D220/D204): correlated by request identity +
    // gateway generation, with a defensive timeout so a lost completion never
    // leaves the page pending (PLC-HMI-003 D4).
    struct PendingParamWrite {
        quint64 request_id = 0;
        quint64 gateway_generation = 0;
        quint16 address = 0;
        qint64 deadline_ms = 0;
    };
    QVector<PendingParamWrite> m_pendingParamWrites;
    void submitParameterWrite(quint16 address, quint16 value);
    void failAllPendingParamWrites(const QString &reason);
    // Gateway generation: incremented before a gateway replacement; events
    // from older generations are rejected (PLC-HMI-003 D4).
    quint64 m_gatewayGeneration = 0;
    bool acceptGatewayGeneration(quint64 generation) const;
    // D204 flow: value waiting for the password verification result.
    bool m_d204Pending = false;
    bool m_d204Cancelled = false;
    qint64 m_d204PendingUserId = -1;
    quint16 m_d204Value = 0;

    // Audit paging bookkeeping (spec §12 滚动加载).
    int m_auditLoadedCount = 0;
};

} // namespace hlm
