#pragma once

#include <QObject>
#include <QString>

#include "domain/barcode_result.h"

namespace hlm {

// Transport-neutral barcode source (contract: "Reserve a transport-neutral
// barcode source boundary …", IBarcodeSource with forbidden_fields
// [vendor_sdk_handle, QWidget_pointer, QSqlDatabase, QModbusClient_pointer, …]).
//
// ACTIVATION (user decision 2026-09-22, revised the same day): the HMI itself
// drives the scan. On the PLC's M15 扫码结束 rising edge the composition root
// submits one decode cycle and the adapter reports its terminal outcome. The
// scope stays exactly what the port reserves:
//   - the vendor SDK handle never appears here (forbidden_fields:
//     vendor_sdk_handle): it lives inside the adapter, behind an internal,
//     testable facade — the same isolation hlm_vision gives OpenCV;
//   - the adapter's blocking SDK calls and its file writes run on the
//     adapter's OWN worker thread, never on the UI thread and never in the PLC
//     control path (contract forbidden_change: no scanner SDK calls, QModbus
//     calls, SQL or blocking waits in QWidget code or the UI thread);
//   - the cycle is independent of the H3U RTU gateway: a scanner failure can
//     never remove the ability to submit Stop or software emergency-stop.
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

    // The configured result-file path (append target). Empty means 未配置: the
    // UI must say so rather than pretend the feature works.
    virtual void setResultPath(const QString &path) = 0;
    virtual QString resultPath() const = 0;

    // Deployment paths, set from the persisted settings (user decision
    // 2026-09-23). These are plain PATH STRINGS, not handles: the
    // `forbidden_fields: vendor_sdk_handle` rule still holds, because the
    // loaded module and its exports never leave the adapter.
    //
    // DLL path: empty = load the vendor library by name (the executable's own
    // directory is searched first); non-empty = load exactly that file.
    virtual void setDllPath(const QString &path) = 0;
    virtual QString dllPath() const = 0;
    // Forward-program path: empty = the step does not run at all. Non-empty =
    // after each cycle that decoded at least one barcode, run that program once
    // with the cycle's barcodes as a single space-joined argument.
    virtual void setForwardExePath(const QString &path) = 0;
    virtual QString forwardExePath() const = 0;

    // A scan cycle ended (the PLC raised M15 扫码结束): run one trigger →
    // poll → decode-result cycle and report through resultReady(). Asynchronous;
    // returns immediately.
    //
    // Returns false when the cycle was NOT submitted because the previous one
    // has not finished. The caller must surface that as a visible state rather
    // than assume a read is coming — an overlapping cycle is never queued,
    // because the SDK itself rejects a second trigger while one is pending.
    virtual bool requestRead() = 0;

    // True while a submitted cycle is still being polled. Lets the caller (and
    // its tests) tell "scanning" from "idle" without guessing.
    virtual bool cycleInProgress() const = 0;

signals:
    // Terminal outcome of one requestRead(), exactly once per submitted cycle.
    // A NoCode/Failed outcome is a result, not silence.
    void resultReady(const hlm::BarcodeResult &result);
};

} // namespace hlm
