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
#include "domain/barcode_result.h"
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
class IBarcodeSource;
class IVisionService;
class LifecycleController;
class UsersSettingsPage;
class RecipeWidthPage;
class ManualControlPage;
class AlarmPage;
class AuditLogPage;
class DiagnosticsPage;
class OverviewPage;
class ScanServicePage;

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
    // 扫码 (user decision 2026-09-22; its own page added 2026-09-23): the
    // result-file path, the scanner program path and the forward-program path are
    // persisted settings; the scan cycle is driven by the PLC's M15 扫码结束
    // coil, and the 扫码服务 page's 采集条码 button exists for bench work.
    void handleBarcodePathSave(const QString &path);
    void handleBarcodePathSaved(bool ok, const QString &error);
    void handleBarcodeSdkPathSave(const QString &path);
    void handleBarcodeForwardExePathSave(const QString &path);
    // Submits one scan cycle on behalf of `source` (M15 扫码结束 or the manual
    // button). A refused overlap is surfaced visibly, never dropped.
    void submitScanCycle(const QString &source);
    void handleBarcodeResult(const BarcodeResult &result);
    void failPendingBarcodePathSave(const QString &reason);
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
    OverviewPage *m_overviewPage = nullptr;
    ScanServicePage *m_scanPage = nullptr;

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
    int m_pendingSettingLoads = 0;
    SerialConnectionSettings m_loadedSerialCfg;

    // 扫码结果源 (user decision 2026-09-22). Owned, no parent: it moves to its
    // own worker thread, exactly like DatabaseService/VisionService.
    IBarcodeSource *m_barcodeSource = nullptr;
    // Configured result-file path (empty = 未配置). Loaded from app_settings at
    // startup and echoed to the settings page and the overview page.
    QString m_barcodePath;
    // 扫码服务块 (user decision 2026-09-23): the vendor library path (empty =
    // load by name from the executable's directory) and the forward program
    // path (empty = do not forward).
    QString m_barcodeSdkPath;
    QString m_barcodeForwardExePath;
    // Single-flight guard across ALL three persisted settings: the DB reports
    // settingSaved() without echoing a key, so at most one save may be in flight
    // for the correlation to be unambiguous (contract: never correlate only by
    // FIFO position when overlapping requests can occur). Adding a second
    // producer is what made this a shared guard rather than a per-setting one.
    bool m_barcodePathSavePending = false;
    // Value being saved (settingSaved() carries no key, so the single in-flight
    // save's value is remembered here to render the confirmed echo).
    QString m_pendingBarcodePath;
    // Which of the three settings the in-flight save belongs to, so its result
    // is rendered on the right editor. `PendingSetting` names it; the value
    // itself is in m_pendingBarcodePath.
    enum class PendingSetting { None, ResultPath, SdkPath, ForwardExePath };
    PendingSetting m_pendingSetting = PendingSetting::None;
    // M15 扫码结束 edge detection on the snapshot feed (rising edge only, and
    // only from a fresh snapshot).
    bool m_lastScanComplete = false;
    // Latest barcode readback, rendered by the overview page. Never a
    // machine-state value: the scanning program is the authoritative peer.
    BarcodeResult m_lastBarcode;

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
    // The same register can be written from more than one page (D220 from the
    // users page and from the 手动控制 page), so each pending write remembers
    // which page asked: the result must surface where the operator clicked
    // (user decision 2026-09-22), never on the other page.
    enum class ParamWriteSink { Users, Manual };
    struct PendingParamWrite {
        quint64 request_id = 0;
        quint64 gateway_generation = 0;
        quint16 address = 0;
        quint16 value = 0; // the written value, for the success detail
        qint64 deadline_ms = 0;
        ParamWriteSink sink = ParamWriteSink::Users;
    };
    QVector<PendingParamWrite> m_pendingParamWrites;
    void submitParameterWrite(quint16 address, quint16 value,
                              ParamWriteSink sink = ParamWriteSink::Users);
    void failAllPendingParamWrites(const QString &reason);
    // Routes a parameter-write outcome to the page that requested it.
    void reportParamWriteResult(const PendingParamWrite &pending, bool ok,
                                const QString &detail);
    // 手动控制 → D220 调宽速度 write (user decision 2026-09-22).
    void handleManualWidthSpeedWrite(quint16 value);
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
