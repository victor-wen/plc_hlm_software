#pragma once

// SDK-based barcode source (user decision 2026-09-22, revised the same day).
//
// One scan cycle, entirely on this adapter's worker thread:
//   1. no configured append path → visibly 未配置, nothing is triggered;
//   2. BR_GetStatusW → remember serverId;
//   3. BR_TriggerW(new requestId) → accepted/pending;
//   4. every kPollIntervalMs, BR_GetResultW(SAME requestId) until the state is
//      completed/failed, or the kCycleDeadlineMs budget runs out;
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
// wrapper over BarcodeReaderTrigger.dll, elsewhere a stub that reports every
// call as unavailable. Declared here so the composition root never needs to
// know which one it got.
std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk();

class BarcodeReaderSdkSource : public IBarcodeSource
{
    Q_OBJECT

public:
    // `sdk` is caller-owned and never deleted or reparented here (the same rule
    // as AppConfig::plcGateway / serialPortDiscovery). Passing nullptr builds
    // the production façade, which reports "unavailable" on any machine
    // without the DLL rather than going silent.
    explicit BarcodeReaderSdkSource(IBarcodeSdk *sdk = nullptr,
                                    QObject *parent = nullptr);
    ~BarcodeReaderSdkSource() override;

    void start() override;
    void stop() override;

    void setResultPath(const QString &path) override;
    QString resultPath() const override;

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
    // *keepPolling says whether a poll is expected at all.
    bool applyReply(const BarcodeSdkReply &reply, BarcodeResult *result,
                    bool *keepPolling);
    // Appends one line per decoded barcode to the configured path. Never
    // touches state: a failed write must not hide a decoded barcode.
    void persistRows(BarcodeResult *result);
    void emitTerminal(const BarcodeResult &result);
    static QString transportReason(BarcodeSdkStatus status);

    IBarcodeSdk *m_sdk = nullptr;
    std::unique_ptr<IBarcodeSdk> m_ownedSdk;

    mutable QMutex m_mutex; // guards m_path / m_cycleInProgress
    QString m_path;
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
