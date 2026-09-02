#pragma once

// Repository port interfaces (spec §7.3, §11.5, §12).
//
// The repositories are plain classes executed exclusively on the SQLite worker
// thread (spec §7.3): all repository operations, migrations and retention
// cleanup run serially on that thread, and the QSqlDatabase connection is never
// shared across threads. UI queries go through the async DatabaseService facade
// (src/adapters/sqlite/database_service.h), never through these interfaces
// directly.
//
// All records are immutable value types so they can cross the thread boundary
// safely (spec §7.5).

#include <QDateTime>
#include <QString>
#include <QVector>

#include <optional>

#include "application/permission_policy.h" // Role

namespace hlm {

// --- users (spec §11.5) -----------------------------------------------------

struct UserRecord {
    qint64 id = -1;
    QString username;
    Role role = Role::Operator;
    QString passwordHash; // hex digest of the PBKDF2 output, never plaintext
    QByteArray salt;      // 16 random bytes
    int iterations = 0;   // PBKDF2 iteration count
    bool enabled = true;
    QDateTime createdAt;
    QDateTime updatedAt;
};

// Result of an authentication attempt (spec §11.5). `user` is set only on
// success. `reason` is one of: "unknown user", "disabled", "locked",
// "bad credentials".
struct LoginResult {
    bool ok = false;
    QString reason;
    std::optional<UserRecord> user;
};

// --- recipes (spec §10.3) ---------------------------------------------------

struct RecipeRecord {
    qint64 id = -1;
    QString name;
    int targetWidthMm = 0; // 50-400 (CHECK constraint in schema)
    QString createdBy;
    QString updatedBy;
    QDateTime createdAt;
    QDateTime updatedAt;
};

// --- alarms (spec §12) ------------------------------------------------------

enum class AlarmSource { Plc, Hmi };
enum class AlarmSeverity { Info, Warning, Critical };

struct AlarmEventRecord {
    qint64 id = -1;
    AlarmSource source = AlarmSource::Plc;
    quint16 code = 0;
    QString messageSnapshot;
    AlarmSeverity severity = AlarmSeverity::Warning;
    QDateTime startedAt;
    QDateTime endedAt; // invalid => event still active
    quint64 snapshotSequence = 0;

    bool isActive() const { return !endedAt.isValid(); }
};

// --- audit (spec §12) -------------------------------------------------------

enum class AuditResult { Success, Failure };

struct AuditRecord {
    qint64 id = -1;
    QDateTime occurredAt;
    QString username; // "anonymous" when not logged in
    Role role = Role::Anonymous;
    QString action;
    QString target;
    QString redactedParameters; // never contains passwords or tokens
    AuditResult result = AuditResult::Success;
    QString reason;
};

// --- settings (spec §12) ----------------------------------------------------

struct SettingRecord {
    QString key;
    QString typedValue;
    QString updatedBy;
    QDateTime updatedAt;
};

// --- repository ports -------------------------------------------------------

class UserRepository
{
public:
    virtual ~UserRepository() = default;

    virtual bool createUser(const UserRecord &u, QString *error = nullptr) = 0;
    virtual bool updateUser(const UserRecord &u, QString *error = nullptr) = 0;
    virtual bool deleteUser(qint64 id, QString *error = nullptr) = 0;
    virtual std::optional<UserRecord> findByName(const QString &username) const = 0;
    virtual QVector<UserRecord> allUsers() const = 0;
    virtual qint64 countUsers() const = 0;
};

class RecipeRepository
{
public:
    virtual ~RecipeRepository() = default;

    // Inserts when r.id < 0, otherwise updates the row with that id.
    virtual bool saveRecipe(const RecipeRecord &r, QString *error = nullptr) = 0;
    virtual bool deleteRecipe(qint64 id, QString *error = nullptr) = 0;
    virtual std::optional<RecipeRecord> findByName(const QString &name) const = 0;
    virtual QVector<RecipeRecord> allRecipes() const = 0;
};

class SettingsRepository
{
public:
    virtual ~SettingsRepository() = default;

    // Upsert by key.
    virtual bool setSetting(const SettingRecord &s, QString *error = nullptr) = 0;
    virtual std::optional<SettingRecord> getSetting(const QString &key) const = 0;
};

class AlarmRepository
{
public:
    virtual ~AlarmRepository() = default;

    // Inserts a new alarm event. Returns the new row id, or -1 on failure.
    virtual qint64 startAlarm(const AlarmEventRecord &e, QString *error = nullptr) = 0;
    // Marks the event ended. Returns false if the event is unknown or already ended.
    virtual bool endAlarm(qint64 id, quint64 endSequence, QString *error = nullptr) = 0;
    // The currently active event for a source, if any (spec §12: at most one
    // active event per source).
    virtual std::optional<AlarmEventRecord> activeAlarm(AlarmSource source) const = 0;
    virtual QVector<AlarmEventRecord> recentAlarms(int limit) const = 0;
    // Deletes ended events older than `cutoff`. Active events are never
    // deleted (spec §12). Returns the number of removed rows.
    virtual bool purgeEndedBefore(const QDateTime &cutoff, qint64 *removed = nullptr,
                                  QString *error = nullptr) = 0;
};

class AuditRepository
{
public:
    virtual ~AuditRepository() = default;

    virtual bool append(const AuditRecord &a, QString *error = nullptr) = 0;
    virtual QVector<AuditRecord> recent(int limit) const = 0;
    virtual bool purgeBefore(const QDateTime &cutoff, qint64 *removed = nullptr,
                             QString *error = nullptr) = 0;
};

} // namespace hlm

Q_DECLARE_METATYPE(hlm::UserRecord)
Q_DECLARE_METATYPE(hlm::LoginResult)
Q_DECLARE_METATYPE(hlm::RecipeRecord)
Q_DECLARE_METATYPE(hlm::AlarmEventRecord)
Q_DECLARE_METATYPE(hlm::AuditRecord)
Q_DECLARE_METATYPE(hlm::AlarmSource)
Q_DECLARE_METATYPE(hlm::AlarmSeverity)
Q_DECLARE_METATYPE(hlm::AuditResult)
Q_DECLARE_METATYPE(QVector<hlm::UserRecord>)
Q_DECLARE_METATYPE(QVector<hlm::RecipeRecord>)
Q_DECLARE_METATYPE(QVector<hlm::AlarmEventRecord>)
Q_DECLARE_METATYPE(QVector<hlm::AuditRecord>)
