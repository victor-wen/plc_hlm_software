#include "app/application.h"

#include <QApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QSerialPort>
#include <QThread>

#include "adapters/modbus/qt_modbus_plc_gateway.h"
#include "adapters/simulator/simulated_plc_gateway.h"
#include "adapters/sqlite/database_service.h"
#include "adapters/vision/vision_service.h"
#include "app/lifecycle_controller.h"
#include "application/control_coordinator.h"
#include "ports/iplc_gateway.h"
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

// Maps a persisted SerialConfig onto the real gateway's Config (spec §8.1).
QtModbusPlcGateway::Config toModbusConfig(const SerialConfig &s)
{
    QtModbusPlcGateway::Config cfg;
    cfg.portName = s.comPort;
    cfg.baudRate = s.baudRate;
    cfg.station = quint8(qBound(1, s.station, 247));
    cfg.stopBits = s.stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop;
    cfg.parity = s.parity == QStringLiteral("偶")
        ? QSerialPort::EvenParity
        : (s.parity == QStringLiteral("奇") ? QSerialPort::OddParity
                                           : QSerialPort::NoParity);
    cfg.timeoutMs = s.timeoutMs;
    cfg.readRetries = s.readRetries;
    return cfg;
}

} // namespace

Application::Application(const AppConfig &config, QObject *parent)
    : QObject(parent)
    , m_cfg(config)
{
    createObjects();
    wireSignals();
}

Application::~Application()
{
    shutdown();
}

void Application::createObjects()
{
    m_shell = new ShellModel(this);
    m_window = new MainWindow;
    m_window->setParent(nullptr); // top-level window

    // Pages are created inside MainWindow; fetch the pointers (spec §11.3).
    m_usersPage = m_window->findChild<UsersSettingsPage *>();
    m_recipePage = m_window->findChild<RecipeWidthPage *>();
    m_manualPage = m_window->findChild<ManualControlPage *>();
    m_alarmPage = m_window->findChild<AlarmPage *>();
    m_auditPage = m_window->findChild<AuditLogPage *>();
    m_diagPage = m_window->findChild<DiagnosticsPage *>();

    // Gateway: in-process simulator by default, real Modbus with --real
    // (spec §14.2). The concrete pointer is kept so commStatsChanged (which
    // exists only on the concrete classes) can be wired.
    if (m_cfg.useSimulatedGateway) {
        m_gw = new SimulatedPlcGateway(this);
    } else {
        m_gw = new QtModbusPlcGateway(toModbusConfig(m_cfg.serial), this);
    }

    // Coordinator: PulseTransport routes into the gateway (spec §8.5).
    ControlCoordinator::PulseTransport transport;
    transport.startPulse = [this](quint16 address) {
        return m_gw->startPulse(address);
    };
    transport.writeHold = [this](quint16 address, bool value) {
        m_gw->writeCoil(address, value, CommandPriority::Normal);
        return true;
    };
    transport.writeCoil = [this](quint16 address, bool value,
                                 CommandPriority priority) {
        m_gw->writeCoil(address, value, priority);
        return true;
    };
    transport.writeRegister = [this](quint16 address, quint16 value,
                                     CommandPriority priority) {
        m_gw->writeRegister(address, value, priority);
        return true;
    };
    m_coordinator = new ControlCoordinator(
        std::move(transport), ControlCoordinator::Config(m_cfg.resetTimeoutSec),
        nullptr, this);

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
}

void Application::wireSignals()
{
    wireGateway(m_gw);

    // --- coordinator -> shell / recipe page ----------------------------------
    connect(m_coordinator, &ControlCoordinator::commandAccepted, this,
            [this](Command cmd) { m_shell->setCommandPending(cmd, true); });
    connect(m_coordinator, &ControlCoordinator::commandPending, this,
            [this](Command cmd) { m_shell->setCommandPending(cmd, true); });
    connect(m_coordinator, &ControlCoordinator::commandRejected, this,
            [this](Command cmd, const QString &) {
                m_shell->setCommandPending(cmd, false);
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
    connect(m_usersPage, &UsersSettingsPage::logoutRequested, m_lifecycle,
            &LifecycleController::onLogoutRequested);
    connect(m_usersPage, &UsersSettingsPage::logoutClearRequested, m_lifecycle,
            &LifecycleController::onLogoutClearRequested);
    connect(m_usersPage, &UsersSettingsPage::addUserRequested, m_db,
            &DatabaseService::addUser);
    connect(m_usersPage, &UsersSettingsPage::changePasswordRequested, m_db,
            &DatabaseService::changePassword);
    connect(m_usersPage, &UsersSettingsPage::deleteUserRequested, m_db,
            &DatabaseService::deleteUser);
    connect(m_usersPage, &UsersSettingsPage::saveSerialConfigRequested, this,
            &Application::persistSerialConfig);
    connect(m_usersPage, &UsersSettingsPage::writeParameterRequested, this,
            &Application::handleParameterWrite);
    connect(m_usersPage, &UsersSettingsPage::d204WriteRequested, this,
            &Application::handleD204Write);

    // --- RecipeWidthPage -> coordinator / database ---------------------------
    connect(m_recipePage, &RecipeWidthPage::applyAdjustRequested, m_coordinator,
            &ControlCoordinator::adjustWidth);
    connect(m_recipePage, &RecipeWidthPage::saveRecipeRequested, this,
            [this](const QString &name, int targetWidthMm) {
                RecipeRecord r;
                r.name = name;
                r.targetWidthMm = targetWidthMm;
                r.createdBy = m_lifecycle ? m_lifecycle->currentUsername()
                                          : QStringLiteral("anonymous");
                r.updatedBy = r.createdBy;
                m_db->saveRecipe(r);
            });
    connect(m_recipePage, &RecipeWidthPage::deleteRecipeRequested, m_db,
            &DatabaseService::deleteRecipe);

    // --- ManualControlPage -> coordinator -------------------------------------
    connect(m_manualPage, &ManualControlPage::manualHoldRequested, m_coordinator,
            &ControlCoordinator::manualHold);
    connect(m_manualPage, &ManualControlPage::manualLatchRequested, m_coordinator,
            &ControlCoordinator::manualLatch);
    connect(m_manualPage, &ManualControlPage::bypassRequested, m_coordinator,
            &ControlCoordinator::bypass);

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
                m_lifecycle->enterRestrictedMode(reason);
            });
    connect(m_db, &DatabaseService::initialAdminNeeded, m_usersPage,
            &UsersSettingsPage::setNeedsInitialAdmin);
    connect(m_db, &DatabaseService::initialAdminCreated, this,
            [this](bool ok, const QString &) {
                if (ok) {
                    m_usersPage->setNeedsInitialAdmin(false);
                    m_db->listUsers();
                }
            });
    connect(m_db, &DatabaseService::loginResult, this,
            &Application::handleLoginResult);
    connect(m_db, &DatabaseService::usersLoaded, m_usersPage,
            &UsersSettingsPage::setUsers);
    connect(m_db, &DatabaseService::recipesLoaded, m_recipePage,
            &RecipeWidthPage::setRecipes);
    connect(m_db, &DatabaseService::recipeSaved, this,
            [this](bool, const QString &) { m_db->listRecipes(); });
    connect(m_db, &DatabaseService::recipeDeleted, this,
            [this](bool, const QString &) { m_db->listRecipes(); });
    connect(m_db, &DatabaseService::settingLoaded, this,
            &Application::handleSettingLoaded);
    connect(m_db, &DatabaseService::settingSaved, this,
            [this](bool ok, const QString &error) {
                --m_pendingSerialSaves;
                if (!ok)
                    m_serialSaveFailed = true;
                if (m_pendingSerialSaves <= 0) {
                    if (m_serialSaveFailed) {
                        m_usersPage->setSerialSaveResult(
                            false, QStringLiteral("数据库写入失败"));
                    } else {
                        rebuildGateway(m_pendingSerialCfg);
                        m_usersPage->setSerialSaveResult(true, QString());
                    }
                    m_serialSaveFailed = false;
                }
            });
    connect(m_db, &DatabaseService::passwordVerified, this,
            &Application::handlePasswordVerified);
    connect(m_db, &DatabaseService::recentAlarmsLoaded, this,
            &Application::handleAlarmsLoaded);
    connect(m_db, &DatabaseService::recentAuditLoaded, this,
            &Application::handleAuditLoaded);

    // --- VisionService -> diagnostics page ------------------------------------
    if (m_vision) {
        connect(m_vision, &VisionService::selfTestPassed, m_diagPage,
                [this](const QString &version) {
                    m_diagPage->setVisionStatus(version, true, QString());
                });
        connect(m_vision, &VisionService::selfTestFailed, m_diagPage,
                [this](const QString &reason) {
                    m_diagPage->setVisionStatus(QString(), false, reason);
                });
    } else {
        m_diagPage->setVisionStatus(QString(), false,
                                    QStringLiteral("视觉模块未启用"));
    }
}

// Wires every gateway signal. Called for the initial gateway and again after a
// serial-config rebuild (spec §8.1).
void Application::wireGateway(IPlcGateway *gw)
{
    connect(gw, &IPlcGateway::snapshotReady, m_coordinator,
            &ControlCoordinator::onSnapshot);
    connect(gw, &IPlcGateway::snapshotReady, m_shell,
            &ShellModel::updateSnapshot);
    connect(gw, &IPlcGateway::snapshotReady, m_db,
            [this](const DeviceSnapshot &s) {
                m_db->feedPlcAlarmSnapshot(s.faultCode(), s.m14(), s.m4(),
                                           s.sequence());
            });
    connect(gw, &IPlcGateway::connectionStateChanged, m_coordinator,
            &ControlCoordinator::onConnectionChanged);
    connect(gw, &IPlcGateway::connectionStateChanged, m_shell,
            &ShellModel::setOnline);
    connect(gw, &IPlcGateway::writeCompleted, this,
            &Application::handleWriteCompleted);
    // The coordinator drives the M43 pulse from writeCompleted (spec §10.3
    // step 4); without this the adjustWidth flow times out (Task 20 review).
    connect(gw, &IPlcGateway::writeCompleted, m_coordinator,
            &ControlCoordinator::onWriteCompleted);

    // commStatsChanged exists on both concrete gateway classes; connect via
    // the concrete pointer (spec §16). A third gateway type would need its
    // own branch here (Task 20 review).
    if (auto *sim = qobject_cast<SimulatedPlcGateway *>(gw)) {
        connect(sim, &SimulatedPlcGateway::commStatsChanged, m_diagPage,
                [this](quint64 seq, int reconn, int failed) {
                    CommStats stats;
                    stats.lastDataAgeMs = 0;
                    stats.sequence = seq;
                    stats.reconnectCount = reconn;
                    stats.failedPolls = failed;
                    m_diagPage->setCommStats(stats);
                });
    } else if (auto *modbus = qobject_cast<QtModbusPlcGateway *>(gw)) {
        connect(modbus, &QtModbusPlcGateway::commStatsChanged, m_diagPage,
                [this](quint64 seq, int reconn, int failed) {
                    CommStats stats;
                    stats.lastDataAgeMs = 0;
                    stats.sequence = seq;
                    stats.reconnectCount = reconn;
                    stats.failedPolls = failed;
                    m_diagPage->setCommStats(stats);
                });
    }
}

void Application::start()
{
    // Startup order (spec §13): database -> gateway -> vision -> session.
    m_db->start();
    m_gw->start();
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
    // Ordered shutdown (spec §13): clear M42/M106-M111 + stop heartbeat, then
    // gateway, database, vision. M100 is never auto-cleared.
    if (m_lifecycle)
        m_lifecycle->shutdown();
    if (m_gw)
        m_gw->stop();
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

// --- serial config persistence (spec §8.1) -----------------------------------

void Application::persistSerialConfig(const SerialConfig &cfg)
{
    m_pendingSerialCfg = cfg;
    m_pendingSerialSaves = 7;
    m_serialSaveFailed = false;
    const QString updatedBy = m_lifecycle ? m_lifecycle->currentUsername()
                                          : QStringLiteral("anonymous");
    auto save = [this, updatedBy](const QString &key, const QString &value) {
        SettingRecord s;
        s.key = key;
        s.typedValue = value;
        s.updatedBy = updatedBy;
        m_db->setSetting(s);
    };
    save(QString::fromLatin1(kSerialComPort), cfg.comPort);
    save(QString::fromLatin1(kSerialStation), QString::number(cfg.station));
    save(QString::fromLatin1(kSerialBaudRate), QString::number(cfg.baudRate));
    save(QString::fromLatin1(kSerialStopBits), QString::number(cfg.stopBits));
    save(QString::fromLatin1(kSerialParity), cfg.parity);
    save(QString::fromLatin1(kSerialTimeoutMs), QString::number(cfg.timeoutMs));
    save(QString::fromLatin1(kSerialReadRetries), QString::number(cfg.readRetries));
}

void Application::handleSettingLoaded(const std::optional<SettingRecord> &setting)
{
    // Count every load (including a missing key) so the echo fires on first
    // run; otherwise m_pendingSerialLoads never reaches 0 (Task 20 review).
    --m_pendingSerialLoads;
    if (!setting) {
        if (m_pendingSerialLoads <= 0)
            m_usersPage->setSerialConfig(m_loadedSerialCfg);
        return;
    }
    const QString &key = setting->key;
    const QString &value = setting->typedValue;
    bool ok = false;
    if (key == QString::fromLatin1(kSerialComPort)) {
        m_loadedSerialCfg.comPort = value;
    } else if (key == QString::fromLatin1(kSerialStation)) {
        const int v = value.toInt(&ok);
        if (ok)
            m_loadedSerialCfg.station = v;
    } else if (key == QString::fromLatin1(kSerialBaudRate)) {
        const int v = value.toInt(&ok);
        if (ok)
            m_loadedSerialCfg.baudRate = v;
    } else if (key == QString::fromLatin1(kSerialStopBits)) {
        const int v = value.toInt(&ok);
        if (ok)
            m_loadedSerialCfg.stopBits = v;
    } else if (key == QString::fromLatin1(kSerialParity)) {
        m_loadedSerialCfg.parity = value;
    } else if (key == QString::fromLatin1(kSerialTimeoutMs)) {
        const int v = value.toInt(&ok);
        if (ok)
            m_loadedSerialCfg.timeoutMs = v;
    } else if (key == QString::fromLatin1(kSerialReadRetries)) {
        const int v = value.toInt(&ok);
        if (ok)
            m_loadedSerialCfg.readRetries = v;
    }
    if (m_pendingSerialLoads <= 0)
        m_usersPage->setSerialConfig(m_loadedSerialCfg);
}

// Rebuilds the gateway with a new serial configuration: stop the old one,
// create the new one, re-wire every signal, start (spec §8.1).
void Application::rebuildGateway(const SerialConfig &cfg)
{
    if (m_gw) {
        m_gw->stop();
        m_gw->deleteLater();
        m_gw = nullptr;
    }
    if (m_cfg.useSimulatedGateway) {
        m_gw = new SimulatedPlcGateway(this);
    } else {
        m_gw = new QtModbusPlcGateway(toModbusConfig(cfg), this);
    }
    wireGateway(m_gw);
    m_gw->start();
}

// --- parameter writes (D122/D220/D204, spec §11.3) ---------------------------

void Application::handleParameterWrite(quint16 address, quint16 value)
{
    // Range validation is done by the page; the write goes through the
    // gateway at Normal priority (spec §11.3).
    m_pendingParamAddrs.append(int(address));
    m_gw->writeRegister(address, value, CommandPriority::Normal);
}

void Application::handleD204Write(quint16 value, const QString &adminPassword)
{
    // D204 修改需再次验证管理员密码 (spec §11.3): verify against the current
    // session user; the result arrives via passwordVerified.
    m_d204Pending = true;
    m_d204Value = value;
    m_db->verifyPassword(m_currentUserId, adminPassword);
}

void Application::handlePasswordVerified(bool ok)
{
    if (!m_d204Pending)
        return;
    m_d204Pending = false;
    if (ok) {
        m_pendingParamAddrs.append(int(kD204));
        m_gw->writeRegister(kD204, m_d204Value, CommandPriority::Normal);
    } else {
        m_usersPage->setParameterWriteResult(false,
                                             QStringLiteral("管理员密码验证失败"));
    }
}

void Application::handleWriteCompleted(quint16 address, bool ok,
                                       const QString &error)
{
    // Feed the result back to the users page for D122/D220/D204 writes
    // (spec §11.2: 无乐观状态). FIFO: results arrive in write order.
    if (!m_pendingParamAddrs.isEmpty()
        && quint16(m_pendingParamAddrs.first()) == address) {
        m_pendingParamAddrs.removeFirst();
        m_usersPage->setParameterWriteResult(ok, error);
    }
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

void Application::onLoginLogoutRequested()
{
    if (m_shell->role() != Role::Anonymous) {
        m_lifecycle->onLogoutRequested();
    } else {
        // 未登录: 切到用户与设置页 (index 6) 显示登录面板.
        m_window->setCurrentPage(6);
    }
}

// --- command routing + EstopRelease interception (spec §10.6) ----------------

void Application::onCommandRequested(Command cmd)
{
    if (cmd == Command::EstopSet) {
        // 已处于软件急停且当前用户是管理员: 确认后解除急停; 否则置急停.
        // 非管理员点击解除会被 coordinator 的权限门控拒绝 (spec §11.4).
        if (m_shell->isEstop() && m_shell->role() == Role::Admin) {
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
    case Command::Start:
        m_coordinator->start();
        break;
    case Command::Stop:
        m_coordinator->stop();
        break;
    default:
        break; // other commands are not routed from the action bar
    }
}

void Application::handleCommandResult(Command cmd, bool ok, const QString &detail)
{
    m_shell->setCommandPending(cmd, false);
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
