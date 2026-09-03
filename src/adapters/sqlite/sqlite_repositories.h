#pragma once

// SQLite repository implementations (spec §7.3, §11.5, §12).
//
// These classes are plain, non-thread-safe classes that MUST be used only on
// the SQLite worker thread, each with the single QSqlDatabase connection that
// thread owns. The DatabaseService facade (database_service.h) enforces this
// by construction.

#include <QSqlDatabase>

#include "ports/repositories.h"

namespace hlm {

class SqliteUserRepository : public UserRepository
{
public:
    explicit SqliteUserRepository(QSqlDatabase db) : m_db(db) {}

    bool createUser(const UserRecord &u, QString *error = nullptr) override;
    bool updateUser(const UserRecord &u, QString *error = nullptr) override;
    bool deleteUser(qint64 id, QString *error = nullptr) override;
    std::optional<UserRecord> findByName(const QString &username) const override;
    QVector<UserRecord> allUsers() const override;
    qint64 countUsers() const override;

private:
    QSqlDatabase m_db;
};

class SqliteRecipeRepository : public RecipeRepository
{
public:
    explicit SqliteRecipeRepository(QSqlDatabase db) : m_db(db) {}

    bool saveRecipe(const RecipeRecord &r, QString *error = nullptr) override;
    bool deleteRecipe(qint64 id, QString *error = nullptr) override;
    std::optional<RecipeRecord> findByName(const QString &name) const override;
    QVector<RecipeRecord> allRecipes() const override;

private:
    QSqlDatabase m_db;
};

class SqliteSettingsRepository : public SettingsRepository
{
public:
    explicit SqliteSettingsRepository(QSqlDatabase db) : m_db(db) {}

    bool setSetting(const SettingRecord &s, QString *error = nullptr) override;
    std::optional<SettingRecord> getSetting(const QString &key) const override;

private:
    QSqlDatabase m_db;
};

class SqliteAlarmRepository : public AlarmRepository
{
public:
    explicit SqliteAlarmRepository(QSqlDatabase db) : m_db(db) {}

    qint64 startAlarm(const AlarmEventRecord &e, QString *error = nullptr) override;
    bool endAlarm(qint64 id, quint64 endSequence, QString *error = nullptr) override;
    std::optional<AlarmEventRecord> activeAlarm(AlarmSource source) const override;
    QVector<AlarmEventRecord> recentAlarms(int limit) const override;
    bool purgeEndedBefore(const QDateTime &cutoff, qint64 *removed = nullptr,
                          QString *error = nullptr) override;

private:
    QSqlDatabase m_db;
};

class SqliteAuditRepository : public AuditRepository
{
public:
    explicit SqliteAuditRepository(QSqlDatabase db) : m_db(db) {}

    bool append(const AuditRecord &a, QString *error = nullptr) override;
    QVector<AuditRecord> recent(int limit, int offset = 0) const override;
    bool purgeBefore(const QDateTime &cutoff, qint64 *removed = nullptr,
                     QString *error = nullptr) override;

private:
    QSqlDatabase m_db;
};

} // namespace hlm
