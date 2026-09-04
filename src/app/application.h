#pragma once

// Application composition root (spec §7, §13, §15.4). Owns every object of
// the running HMI and wires them together. Everything lives on the UI main
// thread except the SQLite worker (DatabaseService) and the vision worker
// (VisionService), which manage their own threads (spec §7.1, §7.3, §7.4).
//
// Responsibilities:
//  - Assemble ShellModel, MainWindow (pages created inside), the PLC gateway
//    (SimulatedPlcGateway by default, QtModbusPlcGateway with --real),
//    ControlCoordinator, DatabaseService, VisionService (when enabled) and
//    LifecycleController.
//  - Wire every signal: gateway -> coordinator/shell/database/diagnostics,
//    coordinator -> shell/recipe page, MainWindow -> coordinator, pages ->
//    database/coordinator, database -> pages/lifecycle.
//  - EstopRelease interception (spec §10.6): an admin confirming release of a
//    latched software estop goes through a QMessageBox before
//    coordinator.estopRelease(); everything else is coordinator.estopSet().
//  - Serial config persistence (spec §8.1): saveSerialConfigRequested persists
//    the 7 serial.* keys to app_settings, then rebuilds the gateway with the
//    new configuration and re-wires all gateway signals.
//  - D204 write (spec §11.3): the admin password is re-verified via
//    DatabaseService::verifyPassword before the register write is issued.
//  - Startup order (spec §13): db.start() -> gw.start() -> vision.start() ->
//    lifecycle.startSessionTimer() -> window.show().
//  - Shutdown (spec §13): lifecycle.shutdown() (clears M42/M106-M111, stops
//    heartbeat) -> gw.stop() -> db.stop() -> vision.stop(). M100 is never
//    auto-cleared.

#include <QObject>
#include <QString>

#include <optional>

#include "app/configuration.h"
#include "ports/repositories.h" // SettingRecord, UserRecord

class QMainWindow;

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
struct SerialConfig;

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
    void rebuildGateway(const SerialConfig &cfg);
    void persistSerialConfig(const SerialConfig &cfg);
    void handleSettingLoaded(const std::optional<SettingRecord> &setting);
    void handleWriteCompleted(quint16 address, bool ok, const QString &error);
    void handleParameterWrite(quint16 address, quint16 value);
    void handleD204Write(quint16 value, const QString &adminPassword);
    void handlePasswordVerified(bool ok);
    void handleLoginResult(const LoginResult &result);
    void handleAuditLoaded(const QVector<AuditRecord> &records);
    void handleAlarmsLoaded(const QVector<AlarmEventRecord> &alarms);
    void handleCommandResult(Command cmd, bool ok, const QString &detail);
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

    // Page pointers (created inside MainWindow; fetched via findChild).
    UsersSettingsPage *m_usersPage = nullptr;
    RecipeWidthPage *m_recipePage = nullptr;
    ManualControlPage *m_manualPage = nullptr;
    AlarmPage *m_alarmPage = nullptr;
    AuditLogPage *m_auditPage = nullptr;
    DiagnosticsPage *m_diagPage = nullptr;

    // Current session user id (for D204 re-verification, spec §11.3).
    qint64 m_currentUserId = -1;

    // Serial config persistence bookkeeping (spec §8.1).
    int m_pendingSerialSaves = 0;
    bool m_serialSaveFailed = false;
    SerialConfig m_pendingSerialCfg;
    int m_pendingSerialLoads = 0;
    SerialConfig m_loadedSerialCfg;

    // Idempotency guard: shutdown() runs from aboutToQuit and again from the
    // destructor; the second call must not re-issue clears against a stopped
    // gateway (Task 20 review).
    bool m_shutdownDone = false;

    // Parameter write routing: FIFO of in-flight D122/D220/D204 addresses, so
    // writeCompleted can be fed back to setParameterWriteResult in order.
    // A queue (not a single slot) so rapid consecutive writes do not drop the
    // first result (Task 20 review).
    QList<int> m_pendingParamAddrs;
    // D204 flow: value waiting for the password verification result.
    bool m_d204Pending = false;
    quint16 m_d204Value = 0;

    // Audit paging bookkeeping (spec §12 滚动加载).
    int m_auditLoadedCount = 0;
};

} // namespace hlm
