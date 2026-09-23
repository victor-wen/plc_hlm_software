#pragma once

// SDK-based barcode source (user decision 2026-09-22, revised the same day).
//
// One scan cycle, entirely on this adapter's worker thread:
//   1. no configured append path → visibly 未配置, nothing is triggered;
//   2. BR_GetStatusW → remember serverId;
//   3. BR_TriggerW(new requestId) → accepted/pending;
//   4. every Config::pollIntervalMs, BR_GetResultW(SAME requestId) until the
//      state is completed/failed, or the Config::cycleDeadlineMs runs out;
//   5. append each decoded barcode to the configured file, then emit exactly
//      one terminal BarcodeResult.
//
// The structure follows the vendor's own DecodeAsync (TriggerClient.cs:56-77):
// a fresh requestId per cycle, the same id for every retry and query, a 50 ms
// poll cadence, and a hard deadline. One deliberate deviation: the sample only
// recognises state=="completed" and would spin until its deadline on a
// state=="failed" reply; this adapter converges a failed cycle immediately,
// because the contract requires every accepted operation to reach a visible
// terminal state (never silence).
//
// Threading: the SDK calls BLOCK in the calling thread (README_CN.md:30) and
// the append touches disk, so both run here and never on the UI thread. The
// poll is a QTimer rather than a sleep loop, so stop() can quit the thread
// promptly instead of waiting out a cycle.
//
// m_path and m_cycleInProgress are written from the caller thread and read on
// the worker thread; both directions are guarded by m_mutex.

#include <QMutex>
#include <QThread>

#include <memory>

#include "adapters/barcode/barcode_sdk.h"
#include "ports/ibarcode_source.h"

class QElapsedTimer;
class QTimer;

namespace hlm {

// Builds the production SDK façade: on Windows a LoadLibraryW/GetProcAddress
// wrapper over the vendor library, elsewhere a stub that reports every call as
// unavailable. Declared here so the composition root never needs to know which
// one it got.
//
// `dllPath` empty loads the library BY NAME, so Windows searches the running
// executable's own directory first (the documented deployment). Non-empty loads
// exactly that file (user decision 2026-09-23: the operator can point the HMI at
// a DLL kept elsewhere).
std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk(const QString &dllPath = QString());

// Poll cadence, per-cycle budget and forward-program budget for one scan cycle.
// The first two defaults are the vendor sample's (TriggerClient.cs:62,73). They
// are injectable so a test can prove the deadline path in milliseconds instead
// of waiting 35 s — the values are otherwise file-local constants no test can
// reach.
//
// Declared at namespace scope rather than nested: a nested type's default
// member initializers may not be used by a default argument of a member
// function of the still-incomplete enclosing class.
struct BarcodeSdkSourceConfig {
    int pollIntervalMs = 50;
    qint64 cycleDeadlineMs = 35000;
    // How long the forward program may run before it is killed and the cycle
    // reports a visible timeout (user decision 2026-09-23). Injectable for the
    // same reason as the deadline above.
    qint64 forwardTimeoutMs = 10000;
};

class BarcodeReaderSdkSource : public IBarcodeSource
{
    Q_OBJECT

public:
    using Config = BarcodeSdkSourceConfig;

    // `sdk` is caller-owned and never deleted or reparented here (the same rule
    // as AppConfig::plcGateway / serialPortDiscovery). Passing nullptr builds
    // the production façade, which reports "unavailable" on any machine
    // without the DLL rather than going silent.
    explicit BarcodeReaderSdkSource(IBarcodeSdk *sdk = nullptr,
                                    Config config = Config(),
                                    QObject *parent = nullptr);
    ~BarcodeReaderSdkSource() override;

    void start() override;
    void stop() override;

    void setResultPath(const QString &path) override;
    QString resultPath() const override;
    void setDllPath(const QString &path) override;
    QString dllPath() const override;
    void setForwardExePath(const QString &path) override;
    QString forwardExePath() const override;

    bool requestRead() override;
    bool cycleInProgress() const override;

private slots:
    // Runs on the worker thread: begins one cycle.
    void performCycle();
    // Runs on the worker thread: one poll of the current cycle.
    void onPollTimeout();

private:
    // Applies one SDK reply to `result`. Returns true when `result` is terminal
    // (caller emits it), false when the cycle must keep polling — in which case
    // *keepPolling says whether a poll is expected at all. `isTriggerReply`
    // distinguishes the two call sites: `busy`/`not_ready` answer a poll as
    // "not finished yet" but a trigger as "nothing was accepted".
    bool applyReply(const BarcodeSdkReply &reply, BarcodeResult *result,
                    bool *keepPolling, bool isTriggerReply);
    // Appends one line per decoded barcode to the configured path. Never
    // touches state: a failed write must not hide a decoded barcode.
    void persistRows(BarcodeResult *result);
    // Runs the configured forward program once for this cycle with the decoded
    // barcodes as one space-joined argument. Never touches state either: a
    // failed forward must not hide a barcode that was decoded (and the file
    // append above has already happened).
    void forwardRows(BarcodeResult *result);
    // Rebuilds the owned SDK when setDllPath() changed the path. Runs on the
    // worker thread at the start of a cycle, so the module is only ever loaded
    // or unloaded between cycles and never under an in-flight call.
    void applyPendingDllPath();
    // The single convergence path for a terminal cycle: append the decoded
    // barcodes to the file, forward them to the configured program, then emit
    // exactly one terminal result. Repeating the three steps at every converge
    // site is what would let one of them be forgotten.
    void finishCycle(BarcodeResult *result);
    void emitTerminal(const BarcodeResult &result);
    static QString transportReason(BarcodeSdkStatus status);

    IBarcodeSdk *m_sdk = nullptr;
    std::unique_ptr<IBarcodeSdk> m_ownedSdk;
    Config m_config;

    mutable QMutex m_mutex; // guards m_path / m_forwardPath / m_dllPath / flags
    QString m_path;
    QString m_forwardPath;
    QString m_dllPath;
    bool m_sdkPathDirty = false;
    bool m_cycleInProgress = false;

    quint64 m_sequence = 0;
    QString m_requestId;
    QString m_serverId;
    QElapsedTimer *m_deadline = nullptr;
    QTimer *m_poll = nullptr;

    QThread *m_ownerThread = nullptr;
    QThread *m_thread = nullptr;
};

} // namespace hlm
