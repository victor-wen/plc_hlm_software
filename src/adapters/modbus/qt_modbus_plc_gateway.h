#pragma once

#include <QObject>
#include <QSerialPort>
#include <QThread>
#include <QTimer>

#include <functional>
#include <optional>

#include "adapters/modbus/modbus_transport.h"
#include "adapters/modbus/pulse_state_machine.h"
#include "adapters/modbus/reconnect_policy.h"
#include "adapters/modbus/request_queue.h"
#include "domain/serial_connection_settings.h"
#include "ports/iplc_gateway.h"

namespace hlm {

// Real PLC gateway (spec §7.2, §8). Facade living in the caller thread; all
// Modbus work happens on a dedicated worker thread that owns the
// QModbusRtuSerialClient (created there, never moved across threads).
//
// Revised submission contract (PLC-HMI-003 D1/D2): submitWriteCoil/
// submitWriteRegister/submitPulse return a structured SubmissionResult with a
// request id unique within the current gateway generation; exactly one
// correlated submissionCompleted() is emitted per accepted submission, or the
// submission is explicitly converged as failed when the link stops, the
// gateway is stopped/replaced, or a defensive confirmation timeout expires.
class ModbusGatewayWorker;

class QtModbusPlcGateway : public IPlcGateway
{
    Q_OBJECT

public:
    // Serial configuration (spec §8.1). Defaults: 8N1, station 1, 9600 baud.
    struct Config {
        QString portName;
        int baudRate = 9600; // 9600 or 19200
        quint8 station = 1;  // 1-247
        QSerialPort::Parity parity = QSerialPort::NoParity;
        QSerialPort::StopBits stopBits = QSerialPort::OneStop;
        int timeoutMs = 200;
        int readRetries = 1; // spec §8.4: read requests may retry once

        // Builds a transport configuration from the transport-neutral serial
        // settings (SerialConnectionSettings.rule, ARCH-005, NF-04). This is
        // the only place the neutral values are mapped to QSerialPort enums:
        //   station/baudRate/timeoutMs/readRetries pass through,
        //   stop_bits 2 -> TwoStop else OneStop,
        //   parity 奇 -> OddParity, 偶 -> EvenParity, otherwise NoParity.
        static Config fromSettings(const SerialConnectionSettings &settings);
    };

    explicit QtModbusPlcGateway(const Config &cfg, QObject *parent = nullptr);
    ~QtModbusPlcGateway() override;

    // Read-only inspection used by integration tests to verify that persisted
    // settings were applied before the real transport starts.
    Config configuration() const { return m_cfg; }

    void start() override;
    void stop() override;
    bool isOnline() const override;
    void setGatewayGeneration(quint64 generation) override;
    quint64 gatewayGeneration() const override;

    SubmissionResult submitWriteCoil(
        quint16 address, bool value,
        CommandPriority priority = CommandPriority::Normal) override;
    SubmissionResult submitWriteRegister(
        quint16 address, quint16 value,
        CommandPriority priority = CommandPriority::Normal) override;
    SubmissionResult submitPulse(quint16 address) override;

private:
    Config m_cfg;
    QThread m_thread;
    ModbusGatewayWorker *m_worker = nullptr;
    bool m_online = false; // mirrored from connectionStateChanged (UI thread)
    bool m_started = false; // guards against double start()
    quint64 m_gatewayGeneration = 0; // assigned by the composition root
};

// Worker: owns the transport, request queue, reconnect policy, polling timers
// and the transfer state machine. Runs entirely on the gateway's worker
// thread. Public submission slots return their structured result so the facade
// can marshal the accept/reject answer across the thread boundary; they double
// as deterministic test hooks (drive directly with a fake transport and an
// injected clock).
class ModbusGatewayWorker : public QObject
{
    Q_OBJECT

public:
    // `transport` may be null: the worker then creates the real
    // QModbusRtuSerialClient transport in its own thread on start().
    explicit ModbusGatewayWorker(const QtModbusPlcGateway::Config &cfg,
                                 IModbusTransport *transport = nullptr,
                                 QObject *parent = nullptr);
    ~ModbusGatewayWorker() override;

    // Test hooks.
    void setNowMs(std::function<qint64()> now);
    void setPollIntervals(int fastMs, int homeMs, int commandMs, int slowMs);
    bool isOnline() const;
    int reconnectDelayMs() const { return m_reconnectTimer->interval(); }
    // Defensive write-confirmation timeout (parameter writes must converge
    // even when a readback never arrives).
    void setWriteConfirmTimeoutMs(qint64 ms) { m_writeConfirmTimeoutMs = ms; }

public slots:
    void start();
    void stop();
    SubmissionResult submitWriteCoil(quint16 address, bool value,
                                     CommandPriority priority);
    SubmissionResult submitWriteRegister(quint16 address, quint16 value,
                                         CommandPriority priority);
    SubmissionResult submitPulse(quint16 address);
    void setGatewayGeneration(quint64 generation);
    void onPollTick();
    void onReconnectTick();
    void onTransferFinished(const TransferResult &result);

signals:
    void snapshotReady(quint64 gatewayGeneration, const DeviceSnapshot &snapshot);
    void connectionStateChanged(quint64 gatewayGeneration, bool online);
    void submissionCompleted(const SubmissionCompletion &completion);
    void commStatsChanged(const PlcCommStats &stats);

private:
    enum class LinkState { Disconnected, Connecting, Online, Offline };

    // Evidence state of one poll block since the current link opened
    // (PLC-HMI-003 D6). NoEvidence ages from the session start: a block that
    // has not succeeded yet is non-valid while its no-success age is inside
    // the approved threshold and Stale once the age crosses it.
    enum class BlockEvidence { NoEvidence, Valid, Failed };

    // One accepted write awaiting readback confirmation, keyed by the queue id
    // of its write request. Carries the port-level identity so the terminal
    // completion matches exactly the submission that produced it.
    struct PendingWrite {
        quint16 address = 0;
        quint16 expected = 0;
        PlcOperation operation = PlcOperation::WriteCoil;
        quint64 submissionId = 0;
        quint64 gatewayGeneration = 0;
        int retriesLeft = 0;
        qint64 deadlineMs = 0; // defensive confirmation timeout
    };

    void openLink();
    void enterOffline();
    void reportDroppedWrites(const QString &reason);
    void scheduleReconnect();
    void tryDispatch();
    void handleReadResult(const ModbusRequest &req, const TransferResult &res);
    void handleWriteResult(const ModbusRequest &req, const TransferResult &res);
    void publishSnapshot();
    // Publishes exactly one snapshot when a block's no-success age crosses its
    // approved stale threshold (PLC-HMI-003 D6): a block without a successful
    // transfer becomes observable as Stale even when it is not the block that
    // drives publication, and no transfer has failed. Called from the poll
    // tick, so the transition is bounded by one tick.
    void publishStaleTransitions();
    // Recomputes the four block ages/qualities from the current evidence and
    // the monotonic last-success timestamps.
    void refreshBlockEvidence(qint64 now);
    static DataQuality evidenceQuality(BlockEvidence evidence, qint64 ageMs,
                                       qint64 staleThresholdMs);
    void checkHeartbeatFreeze(quint16 heartbeat);
    // Emits the one terminal completion for an accepted submission.
    void emitCompletion(quint64 submissionId, quint64 generation,
                        PlcOperation operation, quint16 address, bool ok,
                        const QString &error);
    // Pulse readback path (spec §8.5): a dedicated single-coil readback for
    // the pulse state machine, matched by request identity (isReadback +
    // requestId) but NOT routed through the write-confirmation table.
    void enqueuePulseReadback(quint16 address);
    void handlePulseReadback(const ModbusRequest &req, const TransferResult &res);
    // Expires readback confirmations whose defensive timeout elapsed.
    void expireWriteConfirmations();
    // Transport callbacks for the pulse state machine (spec §8.5): route
    // writes through m_queue.enqueue and completions through the submission
    // identity of the originating pulse.
    static PulseStateMachine::Callbacks makePulseCallbacks(ModbusGatewayWorker *w);

    QtModbusPlcGateway::Config m_cfg;
    IModbusTransport *m_transport = nullptr; // owned (created here or injected)
    bool m_ownsTransport = false;
    bool m_started = false; // guards against double start()

    RequestQueue m_queue;
    ReconnectPolicy m_policy;
    LinkState m_state = LinkState::Disconnected;
    bool m_busy = false;
    bool m_hasValidSnapshot = false;
    quint64 m_sequence = 0;
    std::optional<ModbusRequest> m_inFlight;

    // Gateway generation carried by submissions and events (PLC-HMI-003 D2).
    quint64 m_gatewayGeneration = 0;
    bool m_hasOpened = false;
    // Unique within a generation; never reused while an outcome can arrive.
    quint64 m_nextRequestId = 1;

    // Task 5 pulse state machine, driven from m_pollTimer (spec §7.2, §8.5).
    // Its writeCoil callbacks route through m_queue.enqueue; a rejected
    // enqueue (queue closed = offline) aborts the pulse (spec §8.4 no-replay).
    PulseStateMachine m_pulses;
    // Dedicated pulse readbacks (spec §8.5): single-coil reads matched by
    // request identity, kept separate from the write-confirmation table.
    QHash<quint64, quint16> m_pulseReadbacks; // requestId -> pulse address
    quint64 m_pulseReadbackId = 1;

    // Communication statistics (spec §16).
    quint64 m_reconnectCount = 0;
    quint64 m_failedPolls = 0;

    // Poll scheduling (nominal intervals, spec §8.3).
    int m_fastMs = 250;
    int m_homeMs = 250;
    int m_commandMs = 500;
    int m_slowMs = 1000;
    qint64 m_lastFastMs = 0;
    qint64 m_lastHomeMs = 0;
    qint64 m_lastCommandMs = 0;
    qint64 m_lastSlowMs = 0;
    qint64 m_fastDispatchedMs = 0; // clock time the in-flight fast poll was sent

    // Per-block evidence (PLC-HMI-003 D6): outcome and monotonic time of the
    // last successful transfer for each source block. On openLink() the
    // timestamps start at the session time so a block that never succeeds
    // still ages toward its stale threshold.
    BlockEvidence m_fastEvidence = BlockEvidence::NoEvidence;
    BlockEvidence m_homeEvidence = BlockEvidence::NoEvidence;
    BlockEvidence m_commandEvidence = BlockEvidence::NoEvidence;
    BlockEvidence m_slowEvidence = BlockEvidence::NoEvidence;
    qint64 m_lastFastSuccessMs = 0;
    qint64 m_lastHomeSuccessMs = 0;
    qint64 m_lastCommandSuccessMs = 0;
    qint64 m_lastSlowSuccessMs = 0;

    // Last published per-block quality: a threshold crossing publishes exactly
    // one snapshot, and an unchanged quality is never re-published.
    DataQuality m_publishedFastQuality = DataQuality::ProtocolError;
    DataQuality m_publishedHomeQuality = DataQuality::ProtocolError;
    DataQuality m_publishedCommandQuality = DataQuality::ProtocolError;
    DataQuality m_publishedSlowQuality = DataQuality::ProtocolError;

    // D140 heartbeat freeze detection (spec §8.4).
    quint16 m_lastHeartbeat = 0;
    bool m_haveHeartbeat = false;
    qint64 m_lastHeartbeatChangeMs = 0;

    // Defensive write-confirmation timeout (spec §13, PLC-HMI-003 D2): a
    // parameter write whose readback never arrives still converges.
    qint64 m_writeConfirmTimeoutMs = 5000;

    // Accumulated snapshot data (fast block refreshed every 250 ms).
    DeviceSnapshotData m_data;
    // Pending write confirmations keyed by the queue id of the original write
    // request, so duplicate writes to the same address each get their own
    // confirmation (spec §8.4) and their own submission identity.
    QHash<quint64, PendingWrite> m_pendingConfirmations;

    std::function<qint64()> m_nowMs;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;
};

} // namespace hlm
