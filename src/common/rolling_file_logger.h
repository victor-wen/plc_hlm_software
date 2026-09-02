#pragma once

// Rolling diagnostic log (spec §12, §17).
//
// - Defaults: at most 10 files of 10 MiB each. When the current file exceeds
//   the size limit it is rotated; the oldest file is deleted.
// - Sensitive-data redaction: passwords, password digests and session tokens
//   are replaced with "[REDACTED]" before anything is written (spec §17).
// - Thread-safe: a mutex serializes writers; the logger may be used from any
//   thread.

#include <QFile>
#include <QMutex>
#include <QString>

namespace hlm {

inline constexpr int kDefaultLogFileCount = 10;
inline constexpr qint64 kDefaultLogFileSizeBytes = 10 * 1024 * 1024; // 10 MiB

// Replaces sensitive values in `text` with "[REDACTED]". Redaction is
// conservative: it matches the known sensitive field names and the
// "password=..." / "token=..." patterns, and never logs the values.
QString redactSensitive(const QString &text);

class RollingFileLogger
{
public:
    // `dir` must exist; files are named log.1 .. log.N (log.1 newest).
    RollingFileLogger(QString dir, int maxFiles = kDefaultLogFileCount,
                      qint64 maxFileBytes = kDefaultLogFileSizeBytes);
    ~RollingFileLogger();

    // Appends one line (timestamped) after redaction. Never throws.
    void write(const QString &message);

    // Current file index (1-based) and size; used by tests.
    int currentIndex() const;
    qint64 currentSize() const;

private:
    void rotateLocked();
    void openCurrentLocked();

    QString m_dir;
    int m_maxFiles;
    qint64 m_maxFileBytes;
    int m_index = 1;
    QFile m_file;
    mutable QMutex m_mutex;
};

} // namespace hlm
