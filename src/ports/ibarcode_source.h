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
    // `forbidden_fields: vendor_sdk_handle` rule still holds, because whatever
    // runs the scanner program never leaves the adapter.
    //
    // Scanner program path (the vendor CLI): empty = not configured, nothing is
    // run and the surface says so.
    virtual void setScannerProgramPath(const QString &path) = 0;
    virtual QString scannerProgramPath() const = 0;
    // Forward-program path: empty = the step does not run at all. Non-empty =
    // after each cycle that decoded at least one barcode, run that program once
    // with ONE ARGUMENT PER DECODED BARCODE, in table order (user decision
    // 2026-09-23; revised the same day when the real forward program — TCP_HMI
    // V1.0.5 — turned out to build its frame from separate args).
    virtual void setForwardExePath(const QString &path) = 0;
    virtual QString forwardExePath() const = 0;
    // How many barcodes the current recipe expects (user decision 2026-09-23).
    // 0 = do not check. A cycle that decodes a different number converges as
    // CountMismatch and is NOT persisted or forwarded: a partial read is not a
    // board result, and writing it would only pollute the traceability file.
    // The caller (which knows the recipe) decides whether to retry.
    virtual void setExpectedBarcodeCount(int count) = 0;
    virtual int expectedBarcodeCount() const = 0;

    // A scan cycle ended — the PLC raised M11 相机触发中 (user decision
    // 2026-09-23: M11 is what the HMI READS; M15 is what the HMI WRITES back):
    // run one trigger → poll → decode-result cycle and report through
    // resultReady(). Asynchronous; returns immediately.
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
