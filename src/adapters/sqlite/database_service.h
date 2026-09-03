#pragma once

// Async facade over the SQLite repositories (spec §7.3).
//
// The service owns a dedicated worker thread and is moved onto it: the
// QSqlDatabase connection is created on that thread and is exclusively used
// there. All repository operations, migrations and retention cleanup run
// serially on the worker thread via the queued slots below; results arrive via
// signals on the caller's thread (queued connections).
//
// The service must be created WITHOUT a parent so it can be moved to the
// worker thread. Call stop() before destroying it.
//
// Restricted mode (spec §13): if the database cannot be opened or migrations
// fail, the service enters restricted mode and emits databaseRestricted().
// In restricted mode only the online-stop and software-estop capabilities
// remain available (enforced by the application layer, spec §13); the service
// keeps the repositories unavailable.

#include <QObject>
#include <QString>

#include "ports/repositories.h"

class QSqlDatabase;
class QThread;

namespace hlm {

class UserRepository;
class RecipeRepository;
class SettingsRepository;
class AlarmRepository;
class AuditRepository;
class AuthService;
class AlarmEdgeDetector;

class DatabaseService : public QObject
{
    Q_OBJECT

public:
    explicit DatabaseService(QString databasePath, QObject *parent = nullptr);
    ~DatabaseService() override;

    // Creates the worker thread, moves this service onto it and opens the
    // database. Emits ready() or databaseRestricted() from the worker thread.
    void start();

    // Stops the worker thread and closes the connection. Safe to call twice.
    void stop();

    // True once the database is in restricted mode (spec §13). Only meaningful
    // after ready()/databaseRestricted() has been emitted.
    bool isRestricted() const { return m_restricted; }

    // --- async operations (thread-safe; results via signals) ----------------

public slots:
    void createInitialAdmin(const QString &username, const QString &password);
    void login(const QString &username, const QString &password);
    // Pure password verification for the D204 二次验证 flow (spec §11.3):
    // no lockout accounting, no audit. Result via passwordVerified(bool).
    void verifyPassword(qint64 userId, const QString &password);
    // True when no user exists yet (first start, spec §11.5). Result via
    // initialAdminNeeded(bool).
    void needsInitialAdmin();
    void changePassword(qint64 userId, const QString &newPassword);
    void addUser(const QString &username, Role role, const QString &password);
    void deleteUser(qint64 userId);
    void listUsers();
    void saveRecipe(const RecipeRecord &recipe);
    void deleteRecipe(qint64 id);
    void listRecipes();
    void setSetting(const SettingRecord &setting);
    void getSetting(const QString &key);
    void feedPlcAlarmSnapshot(quint16 d110, bool m14, bool m4, quint64 sequence);
    void startHmiAlarm(const QString &message, AlarmSeverity severity, quint64 sequence);
    void endHmiAlarm(quint64 sequence);
    void listRecentAlarms(int limit);
    void appendAudit(const AuditRecord &record);
    void listRecentAudit(int limit, int offset = 0);
    // Daily retention cleanup (spec §12): purges ended alarms and audit rows
    // older than 365 days. Active alarms are never deleted.
    void runRetentionCleanup();

signals:
    void ready();
    void databaseRestricted(const QString &reason);
    void initialAdminCreated(bool ok, const QString &error);
    void initialAdminNeeded(bool needs);
    void passwordVerified(bool ok);
    void loginResult(const LoginResult &result);
    void passwordChanged(bool ok, const QString &error);
    void userAdded(bool ok, const QString &error);
    void userDeleted(bool ok, const QString &error);
    void usersLoaded(const QVector<UserRecord> &users);
    void recipeSaved(bool ok, const QString &error);
    void recipeDeleted(bool ok, const QString &error);
    void recipesLoaded(const QVector<RecipeRecord> &recipes);
    void settingSaved(bool ok, const QString &error);
    void settingLoaded(const std::optional<SettingRecord> &setting);
    void alarmSnapshotProcessed(bool ok, const QString &error);
    void hmiAlarmStarted(bool ok, const QString &error);
    void hmiAlarmEnded(bool ok, const QString &error);
    void recentAlarmsLoaded(const QVector<AlarmEventRecord> &alarms);
    void auditAppended(bool ok, const QString &error);
    void recentAuditLoaded(const QVector<AuditRecord> &records);
    void retentionCleanupDone(qint64 removedAlarms, qint64 removedAudit);

private slots:
    // Runs on the worker thread: opens the connection, applies PRAGMAs and
    // migrations, and builds the repositories (spec §7.3).
    void openDatabase();

private:
    QString m_databasePath;
    bool m_restricted = false;
    QThread *m_thread = nullptr;

    // Owned by the worker thread; null when restricted.
    UserRepository *m_users = nullptr;
    RecipeRepository *m_recipes = nullptr;
    SettingsRepository *m_settings = nullptr;
    AlarmRepository *m_alarms = nullptr;
    AuditRepository *m_audit = nullptr;
    AuthService *m_auth = nullptr;
    AlarmEdgeDetector *m_alarmDetector = nullptr;
};

} // namespace hlm

Q_DECLARE_METATYPE(std::optional<hlm::SettingRecord>)
