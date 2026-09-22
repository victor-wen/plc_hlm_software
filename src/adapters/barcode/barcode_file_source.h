#pragma once

#include <QMutex>
#include <QThread>

#include "ports/ibarcode_source.h"

namespace hlm {

// File-based IBarcodeSource (user decision 2026-09-22): the external scanning
// program writes its results to a text file and the HMI reads the newest line.
//
// Reference format (需求/扫码相关/Barcode.txt): one CRLF-terminated line per
// historical result, '^'-separated fields, e.g.
//     C3003090^M10^260224^002700
// The '^M' in the sample is literal text, not a control character.
//
// All file I/O runs on the adapter's own worker thread. requestRead() is
// queued into that thread, so a slow or unreachable network path can never
// block the UI thread (contract forbidden_change).
//
// The m_path/m_sequence members are written from the caller thread and read on
// the worker thread; both directions are guarded by m_mutex.
class BarcodeFileSource : public IBarcodeSource
{
    Q_OBJECT

public:
    explicit BarcodeFileSource(QObject *parent = nullptr);
    ~BarcodeFileSource() override;

    void start() override;
    void stop() override;

    void setResultPath(const QString &path) override;
    QString resultPath() const override;

    void requestRead() override;

private slots:
    // Runs on the worker thread: reads the file and emits resultReady().
    void performRead();

private:
    mutable QMutex m_mutex; // guards m_path/m_sequence
    QString m_path;
    quint64 m_sequence = 0;

    // Baseline of the last read that returned a line. An unchanged
    // (mtime, size) pair means the scanning program decoded nothing this cycle
    // and left the previous text in place.
    qint64 m_lastModifiedMs = -1;
    qint64 m_lastSize = -1;

    QThread *m_ownerThread = nullptr;
    QThread *m_thread = nullptr;
};

} // namespace hlm