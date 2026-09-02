#include "common/rolling_file_logger.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace hlm {

namespace {

QString fileNameFor(int index)
{
    return QStringLiteral("log.%1").arg(index);
}

} // namespace

QString redactSensitive(const QString &text)
{
    // Field-name based redaction: password, password digest/hash and session
    // token values are never logged (spec §17).
    static const QRegularExpression fieldRe(
        QStringLiteral("(?i)(password|passwd|pwd|password_hash|passwordhash|"
                       "token|session_token|sessiontoken)\\s*([=:])(\\s*)[^\\s,;]+"));
    QString out = text;
    out.replace(fieldRe, QStringLiteral("\\1\\2\\3[REDACTED]"));
    // Bare "password=..." style already covered; also catch a trailing
    // "password" value in key-value dumps like "password: hunter2".
    return out;
}

RollingFileLogger::RollingFileLogger(QString dir, int maxFiles, qint64 maxFileBytes)
    : m_dir(std::move(dir)), m_maxFiles(qMax(1, maxFiles)),
      m_maxFileBytes(qMax<qint64>(1, maxFileBytes))
{
    QDir().mkpath(m_dir);
    // Resume at the highest existing index so a restart does not overwrite
    // the newest file.
    for (int i = m_maxFiles; i >= 1; --i) {
        if (QFileInfo::exists(m_dir + QLatin1Char('/') + fileNameFor(i))) {
            m_index = i;
            break;
        }
    }
    openCurrentLocked();
}

RollingFileLogger::~RollingFileLogger()
{
    QMutexLocker lock(&m_mutex);
    m_file.close();
}

void RollingFileLogger::write(const QString &message)
{
    QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen() && !m_file.open(QIODevice::Append | QIODevice::Text))
        return;
    if (m_file.size() >= m_maxFileBytes)
        rotateLocked();
    const QString line = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
        + QStringLiteral(" ") + redactSensitive(message) + QLatin1Char('\n');
    m_file.write(line.toUtf8());
    m_file.flush();
}

int RollingFileLogger::currentIndex() const
{
    QMutexLocker lock(&m_mutex);
    return m_index;
}

qint64 RollingFileLogger::currentSize() const
{
    QMutexLocker lock(&m_mutex);
    return m_file.size();
}

void RollingFileLogger::rotateLocked()
{
    m_file.close();
    // Delete the oldest file, then shift the rest up by one.
    const QString oldest = m_dir + QLatin1Char('/') + fileNameFor(m_maxFiles);
    if (QFileInfo::exists(oldest) && !QFile::remove(oldest))
        qWarning("RollingFileLogger: failed to remove oldest log file %s",
                 qPrintable(oldest));
    for (int i = m_maxFiles - 1; i >= 1; --i) {
        const QString from = m_dir + QLatin1Char('/') + fileNameFor(i);
        const QString to = m_dir + QLatin1Char('/') + fileNameFor(i + 1);
        if (QFileInfo::exists(from) && !QFile::rename(from, to))
            qWarning("RollingFileLogger: failed to rename %s to %s",
                     qPrintable(from), qPrintable(to));
    }
    m_index = 1;
    openCurrentLocked();
}

void RollingFileLogger::openCurrentLocked()
{
    m_file.setFileName(m_dir + QLatin1Char('/') + fileNameFor(m_index));
    m_file.open(QIODevice::Append | QIODevice::Text);
}

} // namespace hlm
