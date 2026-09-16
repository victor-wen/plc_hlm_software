#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QtGlobal>

#include "domain/device_snapshot.h"

namespace hlm {

// Command priority hint (spec §8.3). Heartbeat was removed with M112
// (PLC-HMI-003 D3/ARCH-002): only Normal, Safety and PulseClear remain.
enum class CommandPriority {
    Normal = 0,     // user writes (queue level 4: other user writes)
    Safety = 1,     // online stop / estop set / continuous-motion clear (level 2)
    PulseClear = 2, // pulse clear requests (level 1)
};

// Operation carried by a submission and its correlated terminal completion
// (PLC-HMI-003 D1).
enum class PlcOperation {
    WriteCoil = 0,
    WriteRegister = 1,
    Pulse = 2,
};

// Result of a submission attempt. A rejected submission carries request_id 0
// and a non-empty immediate_rejection_reason and never emits a completion.
struct SubmissionResult {
    bool accepted = false;
    quint64 request_id = 0;
    quint64 gateway_generation = 0;
    QString immediate_rejection_reason;
};

// Terminal, correlated outcome of exactly one accepted submission. Consumers
// match by (request_id, gateway_generation); a completion for an unknown
// request or an obsolete generation is ignored.
struct SubmissionCompletion {
    quint64 request_id = 0;
    quint64 gateway_generation = 0;
    PlcOperation operation = PlcOperation::WriteCoil;
    quint16 address = 0;
    bool result = false;
    QString error;
};

// Communication statistics emitted by the port itself (contract IPlcGateway
// event_fields): no concrete-gateway cast is needed to obtain them.
struct PlcCommStats {
    quint64 gateway_generation = 0;
    quint64 snapshot_sequence = 0;
    qint64 per_block_age_ms = 0;
    quint64 reconnect_count = 0;
    quint64 failed_polls = 0;
};

// Port interface for the PLC gateway (spec §7.2, §8). Implemented by the real
// Modbus gateway and by the in-process SimulatedPlcGateway. All methods are
// thread-safe: commands are marshalled onto the gateway's own worker thread;
// results and events arrive via signals.
//
// Correlation contract (PLC-HMI-003 D1/D2):
//  - request_id is unique within a gateway generation and never reused while
//    an outcome can still arrive.
//  - every accepted submission emits exactly one correlated terminal
//    completion, or is explicitly converged as communications-lost/replaced
//    during shutdown, reconnect or gateway replacement.
//  - every connection, snapshot, completion and statistics event identifies
//    its gateway generation.
class IPlcGateway : public QObject
{
    Q_OBJECT

public:
    explicit IPlcGateway(QObject *parent = nullptr) : QObject(parent) {}
    ~IPlcGateway() override = default;

    // Lifecycle. start() opens the link and begins polling; stop() closes it.
    virtual void start() = 0;
    virtual void stop() = 0;

    // True when the gateway has a live link and (after reconnect) has already
    // delivered at least one full valid snapshot.
    virtual bool isOnline() const = 0;

    // The composition root increments its generation counter before replacing
    // a gateway and assigns the new value here; implementations may advance it
    // further on their own reconnects. Events carry this generation so stale
    // events from a replaced gateway can be rejected.
    virtual void setGatewayGeneration(quint64 generation) = 0;
    virtual quint64 gatewayGeneration() const = 0;

    // Asynchronous submissions. The terminal outcome arrives via
    // submissionCompleted() for every accepted request.
    virtual SubmissionResult submitWriteCoil(
        quint16 address, bool value,
        CommandPriority priority = CommandPriority::Normal) = 0;
    virtual SubmissionResult submitWriteRegister(
        quint16 address, quint16 value,
        CommandPriority priority = CommandPriority::Normal) = 0;
    virtual SubmissionResult submitPulse(quint16 address) = 0;

signals:
    // Exactly one terminal outcome for every accepted submission.
    void submissionCompleted(const SubmissionCompletion &completion);
    // A complete, immutable device snapshot plus its gateway generation.
    void snapshotReady(quint64 gateway_generation, const DeviceSnapshot &snapshot);
    // Link state plus its gateway generation.
    void connectionStateChanged(quint64 gateway_generation, bool online);
    // Port-level communication statistics (never obtained by a concrete cast).
    void commStatsChanged(const PlcCommStats &stats);
};

// Registers the port value types for queued cross-thread delivery. Called by
// the gateway adapters; safe to call repeatedly.
void registerPlcGatewayMetaTypes();

} // namespace hlm

Q_DECLARE_METATYPE(hlm::CommandPriority)
Q_DECLARE_METATYPE(hlm::PlcOperation)
Q_DECLARE_METATYPE(hlm::SubmissionResult)
Q_DECLARE_METATYPE(hlm::SubmissionCompletion)
Q_DECLARE_METATYPE(hlm::PlcCommStats)
