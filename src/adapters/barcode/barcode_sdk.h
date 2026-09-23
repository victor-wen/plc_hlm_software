#pragma once

// The vendor BarcodeReader Trigger SDK, reduced to the calls this HMI makes and
// expressed as a plain C++ interface (user decision 2026-09-22).
//
// WHY A FACADE: the SDK is a Windows named-pipe DLL loaded with
// LoadLibraryW/GetProcAddress. Everything platform-specific stays in the
// production implementation inside barcode_reader_sdk_source.cpp, and tests
// inject a fake, so the adapter's whole cycle — trigger, poll, parse, persist,
// converge — is exercisable on the Linux dev loop with no DLL present. This
// mirrors how hlm_vision isolates OpenCV; the vendor handle is explicitly a
// forbidden field on the public port (IBarcodeSource), so it never leaks out.
//
// Reference: 需求/扫码相关/BarcodeReader_TriggerSDK_1.1.0_20260911_BarcodeRead/
// (README_CN.md, BarcodeReaderTrigger.h). Do not re-derive the protocol here —
// that header is the authority and this facade mirrors three of its six
// exports.

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include "domain/barcode_result.h"

namespace hlm {

// BR_* transport return codes (BarcodeReaderTrigger.h). BR_OK is TRANSPORT
// SUCCESS ONLY: the returned JSON must still be inspected for ok/code/state.
enum class BarcodeSdkStatus : int {
    Ok = 0,             // BR_OK
    InvalidArgument = 1,
    NotConnected = 2,   // app not running / not authorised / endpoint missing
    Timeout = 3,        // does NOT cancel an accepted decode
    IoError = 4,
    BufferTooSmall = 5, // retry with the size reported in requiredBytes
    ResponseTooLarge = 6,
    // Not an SDK code: this adapter could not load the DLL at all (missing,
    // wrong bitness, blocked). Callers treat it exactly like NotConnected —
    // the operator-facing reason is the same "start the scan program" one.
    LibraryUnavailable = 100,
};

// One call's outcome: the transport status plus the raw UTF-8 JSON body.
struct BarcodeSdkReply {
    BarcodeSdkStatus status = BarcodeSdkStatus::LibraryUnavailable;
    QByteArray json;
};

// Cuts the JSON payload out of an SDK response buffer.
//
// The SDK writes its JSON at the front of a caller-provided buffer and leaves
// the rest as it found it, so the buffer is NUL-PADDED. Handing the whole thing
// to QJsonDocument fails with GarbageAtEnd: Qt's parser does not treat NUL as
// whitespace, so it stops at the first NUL and then reports that the document
// did not end there. The payload is `requiredBytes - 1` bytes (requiredBytes
// includes the NUL — README_CN.md:28).
//
// This is deliberately platform-neutral and NOT inside the #ifdef: a fake SDK
// returns exactly the JSON with no padding, so without a shared, testable
// helper the DLL-less Linux dev loop can never catch a padding mistake — and
// the only place it bites is real Windows hardware.
QByteArray barcodePayloadFromBuffer(const QByteArray &raw, quint32 requiredBytes);

// Collects one cycle's decoded barcodes into the argument list the forward
// program receives: ONE ARGUMENT PER BARCODE, in table order (user decision
// 2026-09-23, revised the same day after the real forward program — TCP_HMI
// V1.0.5 — was supplied). Positions the SDK reported as empty are skipped: they
// are not barcodes, and an empty argument would make that program reject the
// whole frame.
//
// Why one argument per barcode and not one joined string: TCP_HMI's CLI mode
// builds `BARCODE<TAB>argv[1]<TAB>argv[2]…` from its own command line, and the
// receiving side splits that frame on TAB. A single space-joined argument would
// therefore arrive as ONE barcode whose value happens to contain spaces.
//
// Returns false when a non-empty barcode carries a character that protocol
// cannot represent: TAB is its field separator, and CR/LF terminate its frames.
// Refusing (and saying so visibly) is the same trade-off the rest of this
// adapter makes — a visible failure beats a silent wrong value. Platform-neutral
// on purpose, so the Linux dev loop tests the exact production transform.
bool barcodeForwardArguments(const QVector<BarcodeRow> &rows,
                             QStringList *arguments);

class IBarcodeSdk
{
public:
    virtual ~IBarcodeSdk() = default;

    // BR_GetStatusW: project / running / busy / ready / serverId.
    virtual BarcodeSdkReply status() = 0;
    // BR_TriggerW: submit one decode for `requestId`; replies accepted/pending,
    // or the remembered outcome of a repeated request id.
    virtual BarcodeSdkReply trigger(const QString &requestId) = 0;
    // BR_GetResultW: the outcome of that same request id. It never triggers a
    // new decode, so polling with the SAME id is always safe.
    virtual BarcodeSdkReply result(const QString &requestId) = 0;
};

} // namespace hlm
