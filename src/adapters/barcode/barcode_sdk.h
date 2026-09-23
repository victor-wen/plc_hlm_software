#pragma once

// The vendor BarcodeReader Trigger SDK, reduced to the calls this HMI makes and
// expressed as a plain C++ interface.
//
// WHY A FACADE: everything vendor-specific stays behind this interface, so the
// adapter's whole cycle — trigger, poll, parse, persist, forward, converge — is
// exercisable on the Linux dev loop with no vendor binary present, and tests
// inject a fake. The vendor handle is a forbidden field on the public port
// (IBarcodeSource), so it never leaks out.
//
// TRANSPORT (user decision 2026-09-23): the production implementation drives
// the vendor's own CLI, `trigger_client.exe`, instead of loading
// `BarcodeReaderTrigger.dll` in-process. The DLL route failed in the field for
// reasons the operator could not diagnose, while the CLI is the exact thing
// that already worked on that machine — so the HMI runs what the operator runs
// and reads its stdout. Two further consequences, both good: the CLI's exit
// code IS the BR_* transport code (example_c.c returns `rc`), and the
// implementation is plain QProcess, so it is the same code on every platform
// and no `#ifdef _WIN32` is needed.
//
// Reference: 需求/扫码相关/BarcodeReader_TriggerSDK_1.1.0_20260911_BarcodeRead/
// (README_CN.md:114-138 is the CLI's documented usage, example_c.c is its
// source). Do not re-derive the protocol here — those files are the authority.

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
    BufferTooSmall = 5, // the CLI never reports this; kept so the enum mirrors the header
    ResponseTooLarge = 6,
    // Not an SDK code: the scanner program could not be started at all
    // (missing, not executable, blocked). Callers treat it exactly like
    // NotConnected — the operator-facing reason is the same "start the scan
    // program" one.
    ProgramUnavailable = 100,
};

// One call's outcome: the transport status plus the raw UTF-8 JSON body.
struct BarcodeSdkReply {
    BarcodeSdkStatus status = BarcodeSdkStatus::ProgramUnavailable;
    QByteArray json;
};

// Cuts the JSON payload out of an SDK response buffer.
//
// Cuts the JSON payload out of an in-process SDK response buffer.
//
// Kept because the vendor DLL writes its JSON at the front of a caller-provided
// buffer and leaves the rest as it found it, so that buffer is NUL-PADDED, and
// handing the whole thing to QJsonDocument fails with GarbageAtEnd: Qt's parser
// does not treat NUL as whitespace, so it stops at the first NUL and then
// reports that the document did not end there. The payload is
// `requiredBytes - 1` bytes (requiredBytes includes the NUL — README_CN.md:28).
//
// The production transport no longer goes through that buffer (it runs the
// vendor CLI and reads stdout, which `puts()` terminates cleanly), so nothing
// calls this today. It stays as the documented shape of the DLL contract and as
// the helper any future in-process transport must use — the failure it prevents
// is invisible to a fake that returns an exactly-sized QByteArray.
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

// Whether the forward program's own reply counts as acknowledgement.
//
// The supplied program (TCP_HMI V1.0.5) writes the peer's reply into
// `Result.txt` next to itself and expects the peer to answer `OK`
// (its ini's ExpectedReply). Anything else — a different word, an empty file, a
// file that is not there — is NOT an acknowledgement.
//
// Compared case-insensitively after trimming, because a text file carries line
// endings the writer chose and `OK` vs `ok` is not a distinction the downstream
// protocol makes. Platform-neutral and outside any #ifdef: this is a pure data
// transform, so the Linux dev loop tests the exact production decision.
bool forwardAcknowledgedFromReply(const QByteArray &reply);

class IBarcodeSdk
{
public:
    virtual ~IBarcodeSdk() = default;

    // `status`: project / running / busy / ready / serverId.
    virtual BarcodeSdkReply status() = 0;
    // `trigger`: submit one decode for `requestId`; replies accepted/pending, or
    // the remembered outcome of a repeated request id.
    virtual BarcodeSdkReply trigger(const QString &requestId) = 0;
    // `result`: the outcome of that same request id. It never triggers a new
    // decode, so polling with the SAME id is always safe.
    virtual BarcodeSdkReply result(const QString &requestId) = 0;
};

} // namespace hlm
