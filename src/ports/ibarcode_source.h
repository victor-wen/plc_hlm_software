#pragma once

#include <QObject>
#include <QString>

#include "domain/barcode_result.h"

namespace hlm {

// Transport-neutral barcode result source (contract:
//   "Reserve a transport-neutral barcode source boundary based on an external
//    scanner program depositing results in a configurable folder; actual file
//    parsing and forwarding remain inactive until a file contract is supplied."
// ).
//
// ACTIVATION (user decision 2026-09-22): the file contract was supplied as the
// reference result file 需求/扫码相关/Barcode.txt, so the boundary is active.
// The scope stays exactly what the contract reserved:
//   - the scanning program is external and automatic; the HMI never triggers a
//     scan and never calls the BarcodeReaderTrigger SDK (there is no scanner SDK
//     dependency anywhere in this boundary);
//   - the only I/O is reading one text file at a configurable path, on the
//     adapter's own worker thread — never on the UI thread and never in the PLC
//     control path (contract forbidden_change: no scanner SDK calls, QModbus
//     calls, SQL or blocking waits in QWidget code or the UI thread).
class IBarcodeSource : public QObject
{
    Q_OBJECT

public:
    explicit IBarcodeSource(QObject *parent = nullptr) : QObject(parent) {}
    ~IBarcodeSource() override = default;

    // Starts/stops the adapter's worker thread. The service must be created
    // WITHOUT a parent so it can be moved to that thread; call stop() before
    // destroying it.
    virtual void start() = 0;
    virtual void stop() = 0;

    // The configured result-file path. Empty means 未配置: the UI must say so
    // rather than pretend the feature works.
    virtual void setResultPath(const QString &path) = 0;
    virtual QString resultPath() const = 0;

    // A scan cycle ended (the PLC raised M15 扫码结束): read the result file and
    // report through resultReady(). Asynchronous; returns immediately.
    virtual void requestRead() = 0;

signals:
    // Terminal outcome of one requestRead(), exactly once per request. A
    // NoNewResult/Failed outcome is a result, not silence.
    void resultReady(const hlm::BarcodeResult &result);
};

} // namespace hlm