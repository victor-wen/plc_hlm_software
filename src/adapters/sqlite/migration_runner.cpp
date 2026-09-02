#include "adapters/sqlite/migration_runner.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace hlm {

namespace {

// Executes a multi-statement SQL script. QSqlQuery::exec() only runs the first
// statement, so split on ';' at line ends (the schema uses one statement per
// line and no embedded semicolons).
bool execScript(QSqlDatabase &db, const QString &sql, QString *error)
{
    const QStringList statements = sql.split(QLatin1Char(';'));
    for (const QString &stmt : statements) {
        const QString trimmed = stmt.trimmed();
        if (trimmed.isEmpty())
            continue;
        QSqlQuery q(db);
        if (!q.exec(trimmed)) {
            if (error)
                *error = q.lastError().text();
            return false;
        }
    }
    return true;
}

QString checksumOf(const QString &sql)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(sql.toUtf8(), QCryptographicHash::Sha256).toHex());
}

} // namespace

const QVector<Migration> &schemaMigrations()
{
    static const QVector<Migration> migrations = {
        {1, QStringLiteral("initial_schema"),
         QStringLiteral(
             // schema_migrations is created by the runner itself (IF NOT
             // EXISTS) before any migration runs.
             "CREATE TABLE users ("
             " id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " username TEXT NOT NULL UNIQUE,"
             " role TEXT NOT NULL CHECK (role IN ('admin','operator')),"
             " password_hash TEXT NOT NULL,"
             " salt BLOB NOT NULL,"
             " iterations INTEGER NOT NULL,"
             " enabled INTEGER NOT NULL DEFAULT 1,"
             " created_at TEXT NOT NULL,"
             " updated_at TEXT NOT NULL);"
             "CREATE TABLE recipes ("
             " id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " name TEXT NOT NULL UNIQUE,"
             " target_width_mm INTEGER NOT NULL CHECK (target_width_mm BETWEEN 50 AND 400),"
             " created_by TEXT NOT NULL,"
             " updated_by TEXT NOT NULL,"
             " created_at TEXT NOT NULL,"
             " updated_at TEXT NOT NULL);"
             "CREATE TABLE alarm_events ("
             " id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " source TEXT NOT NULL CHECK (source IN ('plc','hmi')),"
             " code INTEGER NOT NULL,"
             " message_snapshot TEXT NOT NULL,"
             " severity TEXT NOT NULL CHECK (severity IN ('info','warning','critical')),"
             " started_at TEXT NOT NULL,"
             " ended_at TEXT,"
             " snapshot_sequence INTEGER NOT NULL);"
             "CREATE TABLE audit_log ("
             " id INTEGER PRIMARY KEY AUTOINCREMENT,"
             " occurred_at TEXT NOT NULL,"
             " username TEXT NOT NULL,"
             " role TEXT NOT NULL CHECK (role IN ('anonymous','operator','admin')),"
             " action TEXT NOT NULL,"
             " target TEXT NOT NULL,"
             " redacted_parameters TEXT NOT NULL,"
             " result TEXT NOT NULL CHECK (result IN ('success','failure')),"
             " reason TEXT NOT NULL);"
             "CREATE TABLE app_settings ("
             " key TEXT PRIMARY KEY,"
             " typed_value TEXT NOT NULL,"
             " updated_by TEXT NOT NULL,"
             " updated_at TEXT NOT NULL);"
             "CREATE INDEX idx_alarm_events_source_active ON alarm_events(source, ended_at);"
             "CREATE INDEX idx_alarm_events_started ON alarm_events(started_at);"
             "CREATE INDEX idx_audit_log_occurred ON audit_log(occurred_at);")},
    };
    return migrations;
}

bool runMigrations(QSqlDatabase &db, const QVector<Migration> &migrations,
                   QString *error)
{
    // Ensure the bookkeeping table exists even before migration 1 runs.
    QSqlQuery createBookkeeping(db);
    if (!createBookkeeping.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schema_migrations ("
            " version INTEGER PRIMARY KEY,"
            " name TEXT NOT NULL,"
            " applied_at TEXT NOT NULL,"
            " checksum TEXT NOT NULL);"))) {
        if (error)
            *error = createBookkeeping.lastError().text();
        return false;
    }

    QSqlQuery appliedQuery(db);
    if (!appliedQuery.exec(QStringLiteral("SELECT version, checksum FROM schema_migrations"))) {
        if (error)
            *error = appliedQuery.lastError().text();
        return false;
    }
    QSet<int> appliedVersions;
    QHash<int, QString> appliedChecksums;
    while (appliedQuery.next()) {
        const int v = appliedQuery.value(0).toInt();
        appliedVersions.insert(v);
        appliedChecksums.insert(v, appliedQuery.value(1).toString());
    }

    for (const Migration &m : migrations) {
        if (appliedVersions.contains(m.version)) {
            // Verify the checksum of already-applied migrations so a tampered
            // migration is detected (spec §12 schema_migrations).
            if (appliedChecksums.value(m.version) != checksumOf(m.sql)) {
                if (error)
                    *error = QStringLiteral("migration %1 checksum mismatch").arg(m.version);
                return false;
            }
            continue;
        }
        if (!db.transaction()) {
            if (error)
                *error = db.lastError().text();
            return false;
        }
        if (!execScript(db, m.sql, error)) {
            db.rollback();
            return false;
        }
        QSqlQuery record(db);
        record.prepare(QStringLiteral(
            "INSERT INTO schema_migrations(version, name, applied_at, checksum)"
            " VALUES (?, ?, ?, ?)"));
        record.addBindValue(m.version);
        record.addBindValue(m.name);
        record.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        record.addBindValue(checksumOf(m.sql));
        if (!record.exec()) {
            if (error)
                *error = record.lastError().text();
            db.rollback();
            return false;
        }
        if (!db.commit()) {
            if (error)
                *error = db.lastError().text();
            db.rollback();
            return false;
        }
    }
    return true;
}

} // namespace hlm
