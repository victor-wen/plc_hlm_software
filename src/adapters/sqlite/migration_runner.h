#pragma once

// Versioned schema migrations (spec §12, §13).
//
// Each migration runs inside its own transaction; a failure rolls the
// transaction back and leaves the database untouched. The runner records the
// applied version and a checksum of the migration SQL in schema_migrations so
// a tampered or reordered migration is detected on the next start.

#include <QString>
#include <QVector>

class QSqlDatabase;

namespace hlm {

struct Migration {
    int version = 0;
    QString name;
    QString sql; // may contain multiple statements
};

// Applies all migrations with version > current schema version, in ascending
// order. Returns false (and rolls back the failing migration) on any error.
// On success the schema_migrations table reflects every applied migration.
bool runMigrations(QSqlDatabase &db, const QVector<Migration> &migrations,
                   QString *error = nullptr);

// The migrations for the current schema version. Kept in one place so the
// runner and the tests agree on the expected schema.
const QVector<Migration> &schemaMigrations();

} // namespace hlm
