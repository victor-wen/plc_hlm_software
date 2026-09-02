#pragma once

// Shared helper for SQLite unit tests: creates a fresh database on a temp
// file (WAL requires a real path, not :memory:) and applies the schema
// migrations. The caller keeps `dir` alive and removes the connection in
// cleanup().

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "adapters/sqlite/migration_runner.h"

namespace hlm_test {

inline QSqlDatabase createMigratedDb(QTemporaryDir &dir, const QString &connName)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    db.setDatabaseName(dir.filePath(QStringLiteral("test.db")));
    if (!db.open())
        return db;
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    q.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    hlm::runMigrations(db, hlm::schemaMigrations());
    return db;
}

} // namespace hlm_test
