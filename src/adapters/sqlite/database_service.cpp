#include "adapters/sqlite/database_service.h"

#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include "adapters/sqlite/alarm_edge_detector.h"
#include "adapters/sqlite/auth_service.h"
#include "adapters/sqlite/migration_runner.h"
#include "adapters/sqlite/sqlite_repositories.h"

namespace hlm {

namespace {

// Retention window (spec §12): alarms and audit are kept 365 days.
inline constexpr int kRetentionDays = 365;

} // namespace

DatabaseService::DatabaseService(QString databasePath, QObject *parent)
    : QObject(parent), m_databasePath(std::move(databasePath))
{
    qRegisterMetaType<LoginResult>();
    qRegisterMetaType<UserRecord>();
    qRegisterMetaType<RecipeRecord>();
    qRegisterMetaType<AlarmEventRecord>();
    qRegisterMetaType<AuditRecord>();
    qRegisterMetaType<AlarmSource>();
    qRegisterMetaType<AlarmSeverity>();
    qRegisterMetaType<AuditResult>();
    qRegisterMetaType<QVector<UserRecord>>();
    qRegisterMetaType<QVector<RecipeRecord>>();
    qRegisterMetaType<QVector<AlarmEventRecord>>();
    qRegisterMetaType<QVector<AuditRecord>>();
    qRegisterMetaType<std::optional<SettingRecord>>();
}

DatabaseService::~DatabaseService()
{
    stop();
}

void DatabaseService::start()
{
    if (m_thread)
        return;
    m_thread = new QThread(this);
    // The service lives on the worker thread: the connection is created and
    // used only there, and all queued slots run serially on that thread
    // (spec §7.3).
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &DatabaseService::openDatabase);
    connect(m_thread, &QThread::finished, this, [this]() {
        // Release the repository handles (they hold QSqlDatabase copies) and
        // the services before removing the connection. Runs on the worker
        // thread, which is the thread that created the connection (spec §7.3).
        delete m_alarmDetector;
        m_alarmDetector = nullptr;
        delete m_auth;
        m_auth = nullptr;
        delete m_audit;
        m_audit = nullptr;
        delete m_alarms;
        m_alarms = nullptr;
        delete m_settings;
        m_settings = nullptr;
        delete m_recipes;
        m_recipes = nullptr;
        delete m_users;
        m_users = nullptr;
        QSqlDatabase::removeDatabase(QStringLiteral("hlm_sqlite_worker"));
    });
    m_thread->start();
}

void DatabaseService::stop()
{
    if (!m_thread)
        return;
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
}

void DatabaseService::openDatabase()
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                               QStringLiteral("hlm_sqlite_worker"));
    db.setDatabaseName(m_databasePath);
    QString error;
    if (!db.open()) {
        error = db.lastError().text();
    } else {
        // WAL, foreign keys and busy timeout (spec §7.3). A failure here must
        // not silently degrade to rollback journal / no FK enforcement, so any
        // failed PRAGMA closes the connection and enters restricted mode.
        QSqlQuery q(db);
        // PRAGMA journal_mode=WAL returns the resulting mode as a result row;
        // exec() succeeds even when WAL cannot be enabled (silent fallback to
        // rollback journal). Read the returned value and require "wal".
        bool walOk = false;
        if (q.exec(QStringLiteral("PRAGMA journal_mode=WAL")) && q.next())
            walOk = q.value(0).toString().toLower() == QStringLiteral("wal");
        if (!walOk
            || !q.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
            || !q.exec(QStringLiteral("PRAGMA busy_timeout=5000"))) {
            error = q.lastError().text();
            if (error.isEmpty())
                error = QStringLiteral("WAL journal mode could not be enabled");
            db.close();
        } else if (!runMigrations(db, schemaMigrations(), &error)) {
            db.close();
        } else {
            m_users = new SqliteUserRepository(db);
            m_recipes = new SqliteRecipeRepository(db);
            m_settings = new SqliteSettingsRepository(db);
            m_alarms = new SqliteAlarmRepository(db);
            m_audit = new SqliteAuditRepository(db);
            m_auth = new AuthService(m_users, m_audit);
            m_alarmDetector = new AlarmEdgeDetector(m_alarms);
            emit ready();
        }
    }
    if (!m_users) {
        m_restricted = true;
        emit databaseRestricted(error);
    }
}

void DatabaseService::createInitialAdmin(const QString &username, const QString &password)
{
    if (!m_auth) {
        emit initialAdminCreated(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_auth->createInitialAdmin(username, password, &error);
    emit initialAdminCreated(ok, error);
}

void DatabaseService::login(const QString &username, const QString &password)
{
    if (!m_auth) {
        emit loginResult({false, QStringLiteral("database restricted"), std::nullopt});
        return;
    }
    emit loginResult(m_auth->login(username, password));
}

void DatabaseService::changePassword(qint64 userId, const QString &newPassword)
{
    if (!m_auth) {
        emit passwordChanged(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_auth->changePassword(userId, newPassword, &error);
    emit passwordChanged(ok, error);
}

void DatabaseService::listUsers()
{
    if (!m_users) {
        emit usersLoaded({});
        return;
    }
    emit usersLoaded(m_users->allUsers());
}

void DatabaseService::addUser(const QString &username, Role role,
                              const QString &password)
{
    if (!m_auth) {
        emit userAdded(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_auth->createUser(username, role, password, &error);
    emit userAdded(ok, error);
}

void DatabaseService::deleteUser(qint64 userId)
{
    if (!m_auth) {
        emit userDeleted(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_auth->deleteUser(userId, &error);
    emit userDeleted(ok, error);
}

void DatabaseService::saveRecipe(const RecipeRecord &recipe)
{
    if (!m_recipes) {
        emit recipeSaved(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_recipes->saveRecipe(recipe, &error);
    emit recipeSaved(ok, error);
}

void DatabaseService::deleteRecipe(qint64 id)
{
    if (!m_recipes) {
        emit recipeDeleted(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_recipes->deleteRecipe(id, &error);
    emit recipeDeleted(ok, error);
}

void DatabaseService::listRecipes()
{
    if (!m_recipes) {
        emit recipesLoaded({});
        return;
    }
    emit recipesLoaded(m_recipes->allRecipes());
}

void DatabaseService::setSetting(const SettingRecord &setting)
{
    if (!m_settings) {
        emit settingSaved(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_settings->setSetting(setting, &error);
    emit settingSaved(ok, error);
}

void DatabaseService::getSetting(const QString &key)
{
    if (!m_settings) {
        emit settingLoaded(std::nullopt);
        return;
    }
    emit settingLoaded(m_settings->getSetting(key));
}

void DatabaseService::feedPlcAlarmSnapshot(quint16 d110, bool m14, bool m4, quint64 sequence)
{
    if (!m_alarmDetector) {
        emit alarmSnapshotProcessed(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_alarmDetector->onPlcSnapshot(d110, m14, m4, sequence);
    if (!ok)
        error = QStringLiteral("alarm repository write failed");
    emit alarmSnapshotProcessed(ok, error);
}

void DatabaseService::startHmiAlarm(const QString &message, AlarmSeverity severity,
                                    quint64 sequence)
{
    if (!m_alarmDetector) {
        emit hmiAlarmStarted(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_alarmDetector->startHmiAlarm(message, severity, sequence);
    if (!ok)
        error = QStringLiteral("alarm repository write failed");
    emit hmiAlarmStarted(ok, error);
}

void DatabaseService::endHmiAlarm(quint64 sequence)
{
    if (!m_alarmDetector) {
        emit hmiAlarmEnded(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_alarmDetector->endHmiAlarm(sequence);
    if (!ok)
        error = QStringLiteral("alarm repository write failed");
    emit hmiAlarmEnded(ok, error);
}

void DatabaseService::listRecentAlarms(int limit)
{
    if (!m_alarms) {
        emit recentAlarmsLoaded({});
        return;
    }
    emit recentAlarmsLoaded(m_alarms->recentAlarms(limit));
}

void DatabaseService::appendAudit(const AuditRecord &record)
{
    if (!m_audit) {
        emit auditAppended(false, QStringLiteral("database restricted"));
        return;
    }
    QString error;
    const bool ok = m_audit->append(record, &error);
    emit auditAppended(ok, error);
}

void DatabaseService::listRecentAudit(int limit)
{
    if (!m_audit) {
        emit recentAuditLoaded({});
        return;
    }
    emit recentAuditLoaded(m_audit->recent(limit));
}

void DatabaseService::runRetentionCleanup()
{
    if (!m_alarms || !m_audit) {
        emit retentionCleanupDone(0, 0);
        return;
    }
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-kRetentionDays);
    qint64 removedAlarms = 0;
    qint64 removedAudit = 0;
    QString alarmError;
    QString auditError;
    const bool alarmsOk = m_alarms->purgeEndedBefore(cutoff, &removedAlarms, &alarmError);
    const bool auditOk = m_audit->purgeBefore(cutoff, &removedAudit, &auditError);
    if (!alarmsOk)
        qWarning("DatabaseService: alarm retention purge failed: %s",
                 qPrintable(alarmError));
    if (!auditOk)
        qWarning("DatabaseService: audit retention purge failed: %s",
                 qPrintable(auditError));
    emit retentionCleanupDone(removedAlarms, removedAudit);
}

} // namespace hlm
