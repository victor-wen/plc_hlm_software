#pragma once

// Barcode source: one scan cycle, entirely on this adapter's worker thread.
//
//   1. no configured scanner program → visibly 未配置, nothing is run;
//   2. `status` → remember serverId;
//   3. `trigger <new requestId>` → accepted/pending;
//   4. every Config::pollIntervalMs, `result <SAME requestId>` until the state
//      is completed/failed, or the Config::cycleDeadlineMs runs out;
//   5. append each decoded barcode to the configured file, hand them to the
//      forward program, then emit exactly one terminal BarcodeResult.
//
// Each step is ONE invocation of the vendor's CLI (see barcode_sdk.h for why the
// CLI and not the DLL), so a cycle that converges on its first poll runs three
// processes: status, trigger, result.
//
// The structure follows the vendor's own DecodeAsync (TriggerClient.cs:56-77):
// a fresh requestId per cycle, the same id for every retry and query, a 50 ms
// poll cadence, and a hard deadline. One deliberate deviation: the sample only
// recognises state=="completed" and would spin until its deadline on a
// state=="failed" reply; this adapter converges a failed cycle immediately,
// because the contract requires every accepted operation to reach a visible
// terminal state (never silence).
//
// Threading: the CLI call BLOCKS its caller and the append touches disk, so both
// run here and never on the UI thread. The poll is a QTimer rather than a sleep
// loop, so stop() can quit the thread promptly instead of waiting out a cycle.
//
// m_path / m_forwardPath / m_scannerProgramPath / m_cycleInProgress are written
// from the caller thread and read on the worker thread; both directions are
// guarded by m_mutex.

#include <QMutex>
#include <QThread>

#include <memory>

#include "adapters/barcode/barcode_sdk.h"
#include "ports/ibarcode_source.h"

class QElapsedTimer;
class QTimer;

namespace hlm {

// Builds the production façade over the vendor's CLI. Declared here so the
// composition root never needs to know which implementation it got.
//
// `scannerProgramPath` is the CLI's full path — the 扫码程序路径 setting. Empty
// means NOT CONFIGURED, and the adapter says so instead of running anything
// (user decision 2026-09-23: the path is what enables the feature, now that the
// in-process DLL is no longer used).
std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk(const QString &scannerProgramPath = QString());

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
    //
    // The default is 15 s, not the 10 s this started at: the supplied forward
    // program (TCP_HMI V1.0.5) budgets ConnectTimeout 5 s + ResponseTimeout
    // 10 s for its own handshake, so a slower-but-correct downstream would have
    // been reported as 外发超时 and killed mid-send. The budget must exceed the
    // program's own, or "timeout" stops meaning "it is stuck".
    qint64 forwardTimeoutMs = 15000;
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
    void setScannerProgramPath(const QString &path) override;
    QString scannerProgramPath() const override;
    void setForwardExePath(const QString &path) override;
    QString forwardExePath() const override;
    void setExpectedBarcodeCount(int count) override;
    int expectedBarcodeCount() const override;

    bool requestRead() override;
    bool cycleInProgress() const override;

    // The operator-facing reason for a transport status. Public because it is
    // the single place that decides what a failure MEANS to the operator, and
    // the tests pin each wording: "the scan program is closed" and "the scanner
    // CLI could not be started" are different problems with different fixes.
    static QString transportReason(BarcodeSdkStatus status);

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
    // Runs the configured forward program once for this cycle with one argument
    // per decoded barcode, then reads the program's own reply so the cycle can
    // say whether the handover was acknowledged. Never touches state either: a
    // failed forward must not hide a barcode that was decoded (and the file
    // append above has already happened).
    void forwardRows(BarcodeResult *result);
    // Reads Result.txt next to the forward program and EMPTIES it (user decision
    // 2026-09-23: the file is the program's own reply channel, so it must not
    // carry this cycle's verdict into the next one). Failure to clear is
    // reported, never swallowed.
    void readAndClearForwardReply(BarcodeResult *result);
    // Rebuilds the owned transport when setScannerProgramPath() changed the
    // path. Runs on the worker thread at the start of a cycle, so the configured
    // program is only ever swapped between cycles, never under an in-flight
    // call.
    void applyPendingScannerProgramPath();
    // The single convergence path for a terminal cycle: append the decoded
    // barcodes to the file, forward them to the configured program, then emit
    // exactly one terminal result. Repeating the three steps at every converge
    // site is what would let one of them be forgotten.
    void finishCycle(BarcodeResult *result);
    void emitTerminal(const BarcodeResult &result);

    IBarcodeSdk *m_sdk = nullptr;
    std::unique_ptr<IBarcodeSdk> m_ownedSdk;
    Config m_config;

    mutable QMutex m_mutex; // guards the paths / m_expectedCount / flags
    QString m_path;
    QString m_forwardPath;
    QString m_scannerProgramPath;
    int m_expectedCount = 0;
    bool m_scannerProgramDirty = false;
    bool m_cycleInProgress = false;

    quint64 m_sequence = 0;
    // Monotonic suffix for the timestamp request id (see makeRequestId): makes
    // two cycles in the same millisecond still get distinct ids.
    quint64 m_idSequence = 0;
    QString m_requestId;
    QString m_serverId;
    QElapsedTimer *m_deadline = nullptr;
    QTimer *m_poll = nullptr;

    QThread *m_ownerThread = nullptr;
    QThread *m_thread = nullptr;
};

} // namespace hlm
