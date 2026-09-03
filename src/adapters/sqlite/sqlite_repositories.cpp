#include "adapters/sqlite/sqlite_repositories.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace hlm {

namespace {

QString roleToText(Role r)
{
    switch (r) {
    case Role::Admin: return QStringLiteral("admin");
    case Role::Operator: return QStringLiteral("operator");
    case Role::Anonymous: return QStringLiteral("anonymous");
    }
    return QStringLiteral("anonymous");
}

Role roleFromText(const QString &s)
{
    if (s == QLatin1String("admin"))
        return Role::Admin;
    if (s == QLatin1String("operator"))
        return Role::Operator;
    return Role::Anonymous;
}

QString sourceToText(AlarmSource s)
{
    return s == AlarmSource::Plc ? QStringLiteral("plc") : QStringLiteral("hmi");
}

AlarmSource sourceFromText(const QString &s)
{
    return s == QLatin1String("hmi") ? AlarmSource::Hmi : AlarmSource::Plc;
}

QString severityToText(AlarmSeverity s)
{
    switch (s) {
    case AlarmSeverity::Info: return QStringLiteral("info");
    case AlarmSeverity::Warning: return QStringLiteral("warning");
    case AlarmSeverity::Critical: return QStringLiteral("critical");
    }
    return QStringLiteral("warning");
}

AlarmSeverity severityFromText(const QString &s)
{
    if (s == QLatin1String("info"))
        return AlarmSeverity::Info;
    if (s == QLatin1String("critical"))
        return AlarmSeverity::Critical;
    return AlarmSeverity::Warning;
}

QString resultToText(AuditResult r)
{
    return r == AuditResult::Success ? QStringLiteral("success") : QStringLiteral("failure");
}

AuditResult resultFromText(const QString &s)
{
    return s == QLatin1String("success") ? AuditResult::Success : AuditResult::Failure;
}

QString nowIso()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

bool setError(QString *error, const QSqlQuery &q)
{
    if (error)
        *error = q.lastError().text();
    return false;
}

} // namespace

// --- users ------------------------------------------------------------------

bool SqliteUserRepository::createUser(const UserRecord &u, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO users(username, role, password_hash, salt, iterations, enabled,"
        " created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(u.username);
    q.addBindValue(roleToText(u.role));
    q.addBindValue(u.passwordHash);
    q.addBindValue(u.salt);
    q.addBindValue(u.iterations);
    q.addBindValue(u.enabled ? 1 : 0);
    q.addBindValue(nowIso());
    q.addBindValue(nowIso());
    if (!q.exec())
        return setError(error, q);
    return true;
}

bool SqliteUserRepository::updateUser(const UserRecord &u, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE users SET role = ?, password_hash = ?, salt = ?, iterations = ?,"
        " enabled = ?, updated_at = ? WHERE id = ?"));
    q.addBindValue(roleToText(u.role));
    q.addBindValue(u.passwordHash);
    q.addBindValue(u.salt);
    q.addBindValue(u.iterations);
    q.addBindValue(u.enabled ? 1 : 0);
    q.addBindValue(nowIso());
    q.addBindValue(u.id);
    if (!q.exec())
        return setError(error, q);
    return true;
}

bool SqliteUserRepository::deleteUser(qint64 id, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM users WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec())
        return setError(error, q);
    return true;
}

std::optional<UserRecord> SqliteUserRepository::findByName(const QString &username) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, username, role, password_hash, salt, iterations, enabled,"
        " created_at, updated_at FROM users WHERE username = ?"));
    q.addBindValue(username);
    if (!q.exec() || !q.next())
        return std::nullopt;
    UserRecord u;
    u.id = q.value(0).toLongLong();
    u.username = q.value(1).toString();
    u.role = roleFromText(q.value(2).toString());
    u.passwordHash = q.value(3).toString();
    u.salt = q.value(4).toByteArray();
    u.iterations = q.value(5).toInt();
    u.enabled = q.value(6).toInt() != 0;
    u.createdAt = QDateTime::fromString(q.value(7).toString(), Qt::ISODateWithMs);
    u.updatedAt = QDateTime::fromString(q.value(8).toString(), Qt::ISODateWithMs);
    return u;
}

QVector<UserRecord> SqliteUserRepository::allUsers() const
{
    QVector<UserRecord> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "SELECT id, username, role, password_hash, salt, iterations, enabled,"
        " created_at, updated_at FROM users ORDER BY username"));
    while (q.next()) {
        UserRecord u;
        u.id = q.value(0).toLongLong();
        u.username = q.value(1).toString();
        u.role = roleFromText(q.value(2).toString());
        u.passwordHash = q.value(3).toString();
        u.salt = q.value(4).toByteArray();
        u.iterations = q.value(5).toInt();
        u.enabled = q.value(6).toInt() != 0;
        u.createdAt = QDateTime::fromString(q.value(7).toString(), Qt::ISODateWithMs);
        u.updatedAt = QDateTime::fromString(q.value(8).toString(), Qt::ISODateWithMs);
        out.push_back(u);
    }
    return out;
}

qint64 SqliteUserRepository::countUsers() const
{
    QSqlQuery q(m_db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM users")) || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

// --- recipes ----------------------------------------------------------------

bool SqliteRecipeRepository::saveRecipe(const RecipeRecord &r, QString *error)
{
    if (r.id < 0) {
        QSqlQuery q(m_db);
        q.prepare(QStringLiteral(
            "INSERT INTO recipes(name, target_width_mm, created_by, updated_by,"
            " created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)"));
        q.addBindValue(r.name);
        q.addBindValue(r.targetWidthMm);
        q.addBindValue(r.createdBy);
        q.addBindValue(r.updatedBy);
        q.addBindValue(nowIso());
        q.addBindValue(nowIso());
        if (!q.exec())
            return setError(error, q);
        return true;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE recipes SET name = ?, target_width_mm = ?, updated_by = ?,"
        " updated_at = ? WHERE id = ?"));
    q.addBindValue(r.name);
    q.addBindValue(r.targetWidthMm);
    q.addBindValue(r.updatedBy);
    q.addBindValue(nowIso());
    q.addBindValue(r.id);
    if (!q.exec())
        return setError(error, q);
    if (q.numRowsAffected() == 0) {
        if (error)
            *error = QStringLiteral("recipe not found");
        return false;
    }
    return true;
}

bool SqliteRecipeRepository::deleteRecipe(qint64 id, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM recipes WHERE id = ?"));
    q.addBindValue(id);
    if (!q.exec())
        return setError(error, q);
    if (q.numRowsAffected() == 0) {
        if (error)
            *error = QStringLiteral("recipe not found");
        return false;
    }
    return true;
}

std::optional<RecipeRecord> SqliteRecipeRepository::findByName(const QString &name) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, name, target_width_mm, created_by, updated_by, created_at,"
        " updated_at FROM recipes WHERE name = ?"));
    q.addBindValue(name);
    if (!q.exec() || !q.next())
        return std::nullopt;
    RecipeRecord r;
    r.id = q.value(0).toLongLong();
    r.name = q.value(1).toString();
    r.targetWidthMm = q.value(2).toInt();
    r.createdBy = q.value(3).toString();
    r.updatedBy = q.value(4).toString();
    r.createdAt = QDateTime::fromString(q.value(5).toString(), Qt::ISODateWithMs);
    r.updatedAt = QDateTime::fromString(q.value(6).toString(), Qt::ISODateWithMs);
    return r;
}

QVector<RecipeRecord> SqliteRecipeRepository::allRecipes() const
{
    QVector<RecipeRecord> out;
    QSqlQuery q(m_db);
    q.exec(QStringLiteral(
        "SELECT id, name, target_width_mm, created_by, updated_by, created_at,"
        " updated_at FROM recipes ORDER BY name"));
    while (q.next()) {
        RecipeRecord r;
        r.id = q.value(0).toLongLong();
        r.name = q.value(1).toString();
        r.targetWidthMm = q.value(2).toInt();
        r.createdBy = q.value(3).toString();
        r.updatedBy = q.value(4).toString();
        r.createdAt = QDateTime::fromString(q.value(5).toString(), Qt::ISODateWithMs);
        r.updatedAt = QDateTime::fromString(q.value(6).toString(), Qt::ISODateWithMs);
        out.push_back(r);
    }
    return out;
}

// --- settings ---------------------------------------------------------------

bool SqliteSettingsRepository::setSetting(const SettingRecord &s, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO app_settings(key, typed_value, updated_by, updated_at)"
        " VALUES (?, ?, ?, ?)"
        " ON CONFLICT(key) DO UPDATE SET typed_value = excluded.typed_value,"
        " updated_by = excluded.updated_by, updated_at = excluded.updated_at"));
    q.addBindValue(s.key);
    q.addBindValue(s.typedValue);
    q.addBindValue(s.updatedBy);
    q.addBindValue(nowIso());
    if (!q.exec())
        return setError(error, q);
    return true;
}

std::optional<SettingRecord> SqliteSettingsRepository::getSetting(const QString &key) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT key, typed_value, updated_by, updated_at FROM app_settings WHERE key = ?"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return std::nullopt;
    SettingRecord s;
    s.key = q.value(0).toString();
    s.typedValue = q.value(1).toString();
    s.updatedBy = q.value(2).toString();
    s.updatedAt = QDateTime::fromString(q.value(3).toString(), Qt::ISODateWithMs);
    return s;
}

// --- alarms ----------------------------------------------------------------

qint64 SqliteAlarmRepository::startAlarm(const AlarmEventRecord &e, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO alarm_events(source, code, message_snapshot, severity,"
        " started_at, ended_at, snapshot_sequence) VALUES (?, ?, ?, ?, ?, NULL, ?)"));
    q.addBindValue(sourceToText(e.source));
    q.addBindValue(e.code);
    q.addBindValue(e.messageSnapshot);
    q.addBindValue(severityToText(e.severity));
    q.addBindValue(e.startedAt.toString(Qt::ISODateWithMs));
    q.addBindValue(e.snapshotSequence);
    if (!q.exec()) {
        setError(error, q);
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool SqliteAlarmRepository::endAlarm(qint64 id, quint64 endSequence, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "UPDATE alarm_events SET ended_at = ?, snapshot_sequence = ?"
        " WHERE id = ? AND ended_at IS NULL"));
    q.addBindValue(nowIso());
    q.addBindValue(static_cast<qulonglong>(endSequence));
    q.addBindValue(id);
    if (!q.exec())
        return setError(error, q);
    return q.numRowsAffected() > 0;
}

std::optional<AlarmEventRecord> SqliteAlarmRepository::activeAlarm(AlarmSource source) const
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, source, code, message_snapshot, severity, started_at, ended_at,"
        " snapshot_sequence FROM alarm_events WHERE source = ? AND ended_at IS NULL"
        " ORDER BY id DESC LIMIT 1"));
    q.addBindValue(sourceToText(source));
    if (!q.exec() || !q.next())
        return std::nullopt;
    AlarmEventRecord e;
    e.id = q.value(0).toLongLong();
    e.source = sourceFromText(q.value(1).toString());
    e.code = static_cast<quint16>(q.value(2).toUInt());
    e.messageSnapshot = q.value(3).toString();
    e.severity = severityFromText(q.value(4).toString());
    e.startedAt = QDateTime::fromString(q.value(5).toString(), Qt::ISODateWithMs);
    const QString ended = q.value(6).toString();
    if (!ended.isEmpty())
        e.endedAt = QDateTime::fromString(ended, Qt::ISODateWithMs);
    e.snapshotSequence = q.value(7).toULongLong();
    return e;
}

QVector<AlarmEventRecord> SqliteAlarmRepository::recentAlarms(int limit) const
{
    QVector<AlarmEventRecord> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, source, code, message_snapshot, severity, started_at, ended_at,"
        " snapshot_sequence FROM alarm_events ORDER BY started_at DESC, id DESC LIMIT ?"));
    q.addBindValue(limit);
    if (!q.exec())
        return out;
    while (q.next()) {
        AlarmEventRecord e;
        e.id = q.value(0).toLongLong();
        e.source = sourceFromText(q.value(1).toString());
        e.code = static_cast<quint16>(q.value(2).toUInt());
        e.messageSnapshot = q.value(3).toString();
        e.severity = severityFromText(q.value(4).toString());
        e.startedAt = QDateTime::fromString(q.value(5).toString(), Qt::ISODateWithMs);
        const QString ended = q.value(6).toString();
        if (!ended.isEmpty())
            e.endedAt = QDateTime::fromString(ended, Qt::ISODateWithMs);
        e.snapshotSequence = q.value(7).toULongLong();
        out.push_back(e);
    }
    return out;
}

bool SqliteAlarmRepository::purgeEndedBefore(const QDateTime &cutoff, qint64 *removed,
                                             QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM alarm_events WHERE ended_at IS NOT NULL"
                             " AND ended_at < ?"));
    q.addBindValue(cutoff.toString(Qt::ISODateWithMs));
    if (!q.exec())
        return setError(error, q);
    if (removed)
        *removed = q.numRowsAffected();
    return true;
}

// --- audit ------------------------------------------------------------------

bool SqliteAuditRepository::append(const AuditRecord &a, QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "INSERT INTO audit_log(occurred_at, username, role, action, target,"
        " redacted_parameters, result, reason) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
    // Default-constructed QStrings are null and would bind as SQL NULL, which
    // violates the NOT NULL columns; normalize to non-null empty strings.
    q.addBindValue(a.occurredAt.toString(Qt::ISODateWithMs));
    q.addBindValue(a.username.isNull() ? QStringLiteral("") : a.username);
    q.addBindValue(roleToText(a.role));
    q.addBindValue(a.action.isNull() ? QStringLiteral("") : a.action);
    q.addBindValue(a.target.isNull() ? QStringLiteral("") : a.target);
    q.addBindValue(a.redactedParameters.isNull() ? QStringLiteral("") : a.redactedParameters);
    q.addBindValue(resultToText(a.result));
    q.addBindValue(a.reason.isNull() ? QStringLiteral("") : a.reason);
    if (!q.exec())
        return setError(error, q);
    return true;
}

QVector<AuditRecord> SqliteAuditRepository::recent(int limit, int offset) const
{
    QVector<AuditRecord> out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
        "SELECT id, occurred_at, username, role, action, target, redacted_parameters,"
        " result, reason FROM audit_log ORDER BY occurred_at DESC, id DESC LIMIT ? OFFSET ?"));
    q.addBindValue(limit);
    q.addBindValue(offset);
    if (!q.exec())
        return out;
    while (q.next()) {
        AuditRecord a;
        a.id = q.value(0).toLongLong();
        a.occurredAt = QDateTime::fromString(q.value(1).toString(), Qt::ISODateWithMs);
        a.username = q.value(2).toString();
        a.role = roleFromText(q.value(3).toString());
        a.action = q.value(4).toString();
        a.target = q.value(5).toString();
        a.redactedParameters = q.value(6).toString();
        a.result = resultFromText(q.value(7).toString());
        a.reason = q.value(8).toString();
        out.push_back(a);
    }
    return out;
}

bool SqliteAuditRepository::purgeBefore(const QDateTime &cutoff, qint64 *removed,
                                        QString *error)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM audit_log WHERE occurred_at < ?"));
    q.addBindValue(cutoff.toString(Qt::ISODateWithMs));
    if (!q.exec())
        return setError(error, q);
    if (removed)
        *removed = q.numRowsAffected();
    return true;
}

} // namespace hlm
