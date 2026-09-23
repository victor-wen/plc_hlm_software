#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace hlm {

// Outcome of one scan cycle (user decision 2026-09-22, revised the same day:
// the HMI now DRIVES the scan instead of reading a file the scanning program
// wrote).
//
// A cycle is: the PLC raises M15 扫码结束 → the HMI submits one trigger to the
// BarcodeReader SDK → the SDK decodes its latest cached frames → the HMI polls
// the result for the SAME requestId. One cycle therefore yields ZERO OR MORE
// barcodes (one per enabled rectangle), which is what the reference result file
// in 需求/扫码相关/Barcode.txt shows: 12 lines = 2 boards x 6 rectangles.
//
// This value is a READBACK, never a machine command, and it is never reported
// as a success before the SDK has actually returned a decoded result.
enum class BarcodeState : quint8 {
    NotConfigured = 0, // no result path configured: visibly 未配置
    Ok,                // the cycle completed and at least one barcode decoded
    // The cycle completed but every enabled rectangle came back empty. This is
    // a real outcome, not a failure: the SDK reports an empty barcode for a
    // position it looked at and did not recognise. Never substitute an older
    // barcode for an empty one (SDK example_c.c warns about exactly this).
    NoCode,
    // A 扫码结束 edge arrived while the previous cycle was still running, so
    // this board's signal was refused rather than queued (the SDK rejects a
    // second trigger while one is pending, and a silent drop would violate the
    // no-silent-rejection rule). Carries no barcode.
    Overlapped,
    Failed, // the trigger was rejected, the poll timed out, the SDK is absent…
};

// One decoded rectangle of a scan cycle. Mirrors the SDK's result row
// (sequence/rectId/cameraId/barcode/format) so the UI never has to re-derive
// which position a code came from.
struct BarcodeRow {
    QString barcode;  // as decoded; may contain '^' and is shown verbatim
    QString format;   // e.g. "DataMatrix"
    QString cameraId; // e.g. "camera-window-2"
    int rectId = 0;
    int sequence = 0; // table order from the SDK
};

// One scan-cycle result. `rows` holds every rectangle the SDK reported for this
// cycle, empty ones included; `line`/`fields` are the FIRST decoded barcode and
// its '^'-separated fields, kept so a single-barcode surface stays simple. The
// field meanings are NOT documented in the supplied reference file, so the UI
// shows the raw text and never invents a label for a field.
struct BarcodeResult {
    BarcodeState state = BarcodeState::NotConfigured;
    QString line;       // first non-empty barcode of this cycle
    QStringList fields; // that line split on '^'
    QVector<BarcodeRow> rows;
    int decodedCount = 0;   // rows with a non-empty barcode
    int emptyPositions = 0; // rows the SDK reported with an empty barcode
    // Operator-facing reason for NoCode/Failed, and the outcome of the two
    // side effects of this cycle. Displaying, appending and forwarding are three
    // SEPARATE steps: a failed append must never hide a decoded barcode, and a
    // failed forward must never hide either the barcode or a successful append
    // (user decision 2026-09-23: forward each board's barcodes to a local
    // program by passing them as one argument).
    QString detail;
    QString persistDetail;
    bool persisted = false;
    // Forward outcome: `forwarded` is true only after the program actually
    // exited 0. Empty forward path means the step did not run at all, so both
    // fields stay at their defaults rather than claiming a delivery.
    QString forwardDetail;
    bool forwarded = false;
    QString requestId; // the cycle's SDK request id, for diagnosis
    QDateTime readAt;
    quint64 sequence = 0; // monotonic: lets the UI tell cycles apart
};

} // namespace hlm

Q_DECLARE_METATYPE(hlm::BarcodeResult)
Q_DECLARE_METATYPE(hlm::BarcodeRow)
