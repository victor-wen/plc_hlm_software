#include "adapters/barcode/barcode_file_source.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>

namespace hlm {

namespace {

// Largest result file the adapter will read. The reference file is a few
// hundred bytes of history; anything near this cap is a misconfigured path
// (a log or a binary), and reading it would stall the worker thread.
constexpr qint64 kMaxResultBytes = 1024 * 1024;

} // namespace

BarcodeFileSource::BarcodeFileSource(QObject *parent)
    : IBarcodeSource(parent)
    , m_ownerThread(QThread::currentThread())
{
}

BarcodeFileSource::~BarcodeFileSource()
{
    stop();
}

void BarcodeFileSource::start()
{
    if (m_thread)
        return;
    m_thread = new QThread;
    // All file I/O runs on this thread (contract forbidden_change: no blocking
    // waits on the UI thread).
    moveToThread(m_thread);
    connect(m_thread, &QThread::finished, this,
            [this]() { moveToThread(m_ownerThread); }, Qt::DirectConnection);
    m_thread->start();
}

void BarcodeFileSource::stop()
{
    if (!m_thread)
        return;
    QThread *worker = m_thread;
    worker->quit();
    worker->wait();
    m_thread = nullptr;
    delete worker;
}

void BarcodeFileSource::setResultPath(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    if (m_path == path)
        return;
    m_path = path;
    // A new path has no baseline: the next read must not be judged against the
    // previous file's (mtime, size).
    m_lastModifiedMs = -1;
    m_lastSize = -1;
}

QString BarcodeFileSource::resultPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_path;
}

void BarcodeFileSource::requestRead()
{
    // Queued into the worker thread; returns immediately on the caller side.
    QMetaObject::invokeMethod(this, &BarcodeFileSource::performRead,
                              Qt::QueuedConnection);
}

void BarcodeFileSource::performRead()
{
    BarcodeResult result;
    {
        QMutexLocker lock(&m_mutex);
        result.sequence = ++m_sequence;
    }
    result.readAt = QDateTime::currentDateTime();

    const QString path = resultPath().trimmed();
    if (path.isEmpty()) {
        // Visibly 未配置: the UI keeps showing the placeholder, never a value.
        result.state = BarcodeState::NotConfigured;
        emit resultReady(result);
        return;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        result.state = BarcodeState::Failed;
        result.detail = QStringLiteral("扫码结果文件不存在");
        emit resultReady(result);
        return;
    }
    const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 size = info.size();
    if (modifiedMs == m_lastModifiedMs && size == m_lastSize) {
        // The scanning program decoded nothing this cycle and left the previous
        // text in place, so this is NOT this board's barcode.
        result.state = BarcodeState::NoNewResult;
        result.detail = QStringLiteral("本轮未读到条码（结果文件未更新）");
        emit resultReady(result);
        return;
    }
    if (size > kMaxResultBytes) {
        result.state = BarcodeState::Failed;
        result.detail = QStringLiteral("扫码结果文件过大，请检查路径设置");
        emit resultReady(result);
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.state = BarcodeState::Failed;
        result.detail = QStringLiteral("扫码结果文件无法读取");
        emit resultReady(result);
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    // Last non-empty line wins: the file is an append-only history, and CRLF
    // must not leak into the displayed value.
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        result.state = BarcodeState::Ok;
        result.line = line;
        result.fields = line.split(QLatin1Char('^'));
        for (QString &field : result.fields)
            field = field.trimmed();
        break;
    }
    if (result.state != BarcodeState::Ok) {
        result.state = BarcodeState::Failed;
        result.detail = QStringLiteral("扫码结果文件为空");
        emit resultReady(result);
        return;
    }

    // Baseline for the next cycle.
    m_lastModifiedMs = modifiedMs;
    m_lastSize = size;
    emit resultReady(result);
}

} // namespace hlm