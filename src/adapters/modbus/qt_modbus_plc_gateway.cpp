#include "adapters/modbus/qt_modbus_plc_gateway.h"

#include <QDateTime>
#include <QMetaObject>
#include <QModbusDataUnit>
#include <QModbusReply>
#include <QModbusRtuSerialClient>
#include <QTimer>

#include "domain/device_snapshot.h"

namespace hlm {

namespace {

// Poll block definitions (spec §8.3). 0-based protocol addresses.
constexpr quint16 kFastStart = 100;    // D100
constexpr quint16 kFastCount = 41;     // D100-D140
constexpr quint16 kHomeStart = 50;     // M50
constexpr quint16 kHomeCount = 4;      // M50-M53
constexpr quint16 kCommandStart = 100; // M100
constexpr quint16 kCommandCount = 12;  // M100-M111 (M112 removed, D3)
constexpr quint16 kSlowStart = 204;    // D204
constexpr quint16 kSlowCount = 20;     // D204-D223

// D140 heartbeat freeze threshold (spec §8.4).
constexpr qint64 kHeartbeatFreezeMs = 3000;

// Read requests may retry once (spec §8.4).
constexpr int kReadRetries = 1;

// Map a CommandPriority onto the request queue class (spec §8.3). Heartbeat is
// gone with M112 (PLC-HMI-003 D3).
RequestClass requestClassFor(CommandPriority priority)
{
    return (priority == CommandPriority::Safety) ? RequestClass::SafetyWrite
         : (priority == CommandPriority::PulseClear) ? RequestClass::PulseClear
                                                     : RequestClass::UserWrite;
}

} // namespace

// Maps the transport-neutral serial settings onto the gateway transport
// configuration. The QSerialPort enum conversion lives only here, inside
// hlm_modbus (SerialConnectionSettings.rule, NF-04); mapping semantics are
// identical to the previous composition-root helper.
QtModbusPlcGateway::Config QtModbusPlcGateway::Config::fromSettings(
    const SerialConnectionSettings &settings)
{
    Config cfg;
    cfg.portName = settings.port_name;
    cfg.baudRate = settings.baud_rate;
    cfg.station = quint8(qBound(1, settings.station, 247));
    cfg.stopBits =
        settings.stop_bits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop;
    cfg.parity = settings.parity == QStringLiteral("偶")
        ? QSerialPort::EvenParity
        : (settings.parity == QStringLiteral("奇") ? QSerialPort::OddParity
                                                   : QSerialPort::NoParity);
    cfg.timeoutMs = settings.timeout_ms;
    cfg.readRetries = settings.read_retries;
    return cfg;
}

// ---------------------------------------------------------------------------
// Real transport over QModbusRtuSerialClient (spec §7.2, §8.1).
// ---------------------------------------------------------------------------
namespace {

class RtuTransport : public IModbusTransport
{
    Q_OBJECT

public:
    explicit RtuTransport(const QtModbusPlcGateway::Config &cfg, QObject *parent = nullptr)
        : IModbusTransport(parent)
        , m_cfg(cfg)
        , m_client(new QModbusRtuSerialClient(this))
    {
        m_client->setConnectionParameter(QModbusDevice::SerialPortNameParameter, m_cfg.portName);
        m_client->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, m_cfg.baudRate);
        m_client->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, QSerialPort::Data8);
        m_client->setConnectionParameter(QModbusDevice::SerialParityParameter, m_cfg.parity);
        m_client->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, m_cfg.stopBits);
        m_client->setTimeout(m_cfg.timeoutMs);
        m_client->setNumberOfRetries(0); // retries handled by the gateway (spec §8.4)
    }

    bool open() override { return m_client->connectDevice(); }
    void close() override { m_client->disconnectDevice(); }
    bool isOpen() const override
    {
        return m_client->state() == QModbusDevice::ConnectedState;
    }

    bool send(const ModbusRequest &req) override
    {
        QModbusDataUnit unit;
        switch (req.kind) {
        case ModbusRequest::Kind::ReadCoils:
            unit = QModbusDataUnit(QModbusDataUnit::Coils, req.address, req.count);
            break;
        case ModbusRequest::Kind::ReadRegisters:
            unit = QModbusDataUnit(QModbusDataUnit::HoldingRegisters, req.address, req.count);
            break;
        case ModbusRequest::Kind::WriteCoil:
            unit = QModbusDataUnit(QModbusDataUnit::Coils, req.address, 1);
            unit.setValue(0, req.value ? 1 : 0);
            break;
        case ModbusRequest::Kind::WriteRegister:
            unit = QModbusDataUnit(QModbusDataUnit::HoldingRegisters, req.address, 1);
            unit.setValue(0, req.value);
            break;
        }

        QModbusReply *reply = nullptr;
        if (req.kind == ModbusRequest::Kind::ReadCoils
            || req.kind == ModbusRequest::Kind::ReadRegisters) {
            reply = m_client->sendReadRequest(unit, m_cfg.station);
        } else {
            reply = m_client->sendWriteRequest(unit, m_cfg.station);
        }
        if (!reply)
            return false;

        connect(reply, &QModbusReply::finished, this, [this, reply, req]() {
            if (reply->error() == QModbusDevice::NoError) {
                emit transferFinished(makeTransferResult(
                    req, true, QString(), reply->result().values()));
            } else {
                emit transferFinished(makeTransferResult(
                    req, false, reply->errorString(), {}));
            }
            reply->deleteLater();
        });
        return true;
    }

private:
    QtModbusPlcGateway::Config m_cfg;
    QModbusRtuSerialClient *m_client = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// QtModbusPlcGateway facade
// ---------------------------------------------------------------------------
QtModbusPlcGateway::QtModbusPlcGateway(const Config &cfg, QObject *parent)
    : IPlcGateway(parent)
    , m_cfg(cfg)
{
    // Value types cross the worker->facade thread boundary via queued
    // connections; the metatypes must be registered or Qt drops the events.
    registerPlcGatewayMetaTypes();

    m_worker = new ModbusGatewayWorker(m_cfg, nullptr, nullptr);
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &ModbusGatewayWorker::snapshotReady, this,
            &QtModbusPlcGateway::snapshotReady);
    connect(m_worker, &ModbusGatewayWorker::connectionStateChanged, this,
            [this](quint64 generation, bool online) {
                m_online = online;
                emit connectionStateChanged(generation, online);
            });
    connect(m_worker, &ModbusGatewayWorker::submissionCompleted, this,
            &QtModbusPlcGateway::submissionCompleted);
    connect(m_worker, &ModbusGatewayWorker::commStatsChanged, this,
            &QtModbusPlcGateway::commStatsChanged);
}

QtModbusPlcGateway::~QtModbusPlcGateway()
{
    stop();
    m_thread.quit();
    m_thread.wait();
}

void QtModbusPlcGateway::start()
{
    if (m_started)
        return; // double start() would leak the old transport and
                // double-connect transferFinished
    m_started = true;
    m_thread.start();
    if (m_gatewayGeneration == 0)
        m_gatewayGeneration = 1;
    QMetaObject::invokeMethod(m_worker, "setGatewayGeneration", Qt::QueuedConnection,
                              Q_ARG(quint64, m_gatewayGeneration));
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

void QtModbusPlcGateway::stop()
{
    if (m_thread.isRunning())
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    // Mirror the offline state so isOnline()/connectionStateChanged() do not
    // stay stale after stop (the worker emits nothing on stop in the
    // not-yet-started case).
    if (m_online) {
        m_online = false;
        emit connectionStateChanged(m_gatewayGeneration, false);
    }
    // Reset the start guard so a stop->start restart actually runs again.
    m_started = false;
}

bool QtModbusPlcGateway::isOnline() const
{
    return m_online;
}

void QtModbusPlcGateway::setGatewayGeneration(quint64 generation)
{
    m_gatewayGeneration = generation;
    if (m_thread.isRunning())
        QMetaObject::invokeMethod(m_worker, "setGatewayGeneration",
                                  Qt::QueuedConnection, Q_ARG(quint64, generation));
}

quint64 QtModbusPlcGateway::gatewayGeneration() const
{
    return m_gatewayGeneration;
}

SubmissionResult QtModbusPlcGateway::submitWriteCoil(quint16 address, bool value,
                                                     CommandPriority priority)
{
    SubmissionResult result;
    result.gateway_generation = m_gatewayGeneration;
    if (!m_thread.isRunning()) {
        result.immediate_rejection_reason = QStringLiteral("gateway not started");
        return result;
    }
    QMetaObject::invokeMethod(m_worker, "submitWriteCoil", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(SubmissionResult, result),
                              Q_ARG(quint16, address), Q_ARG(bool, value),
                              Q_ARG(CommandPriority, priority));
    return result;
}

SubmissionResult QtModbusPlcGateway::submitWriteRegister(quint16 address, quint16 value,
                                                         CommandPriority priority)
{
    SubmissionResult result;
    result.gateway_generation = m_gatewayGeneration;
    if (!m_thread.isRunning()) {
        result.immediate_rejection_reason = QStringLiteral("gateway not started");
        return result;
    }
    QMetaObject::invokeMethod(m_worker, "submitWriteRegister", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(SubmissionResult, result),
                              Q_ARG(quint16, address), Q_ARG(quint16, value),
                              Q_ARG(CommandPriority, priority));
    return result;
}

SubmissionResult QtModbusPlcGateway::submitPulse(quint16 address)
{
    SubmissionResult result;
    result.gateway_generation = m_gatewayGeneration;
    if (!m_thread.isRunning()) {
        result.immediate_rejection_reason = QStringLiteral("gateway not started");
        return result;
    }
    QMetaObject::invokeMethod(m_worker, "submitPulse", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(SubmissionResult, result),
                              Q_ARG(quint16, address));
    return result;
}

// ---------------------------------------------------------------------------
// ModbusGatewayWorker
// ---------------------------------------------------------------------------

PulseStateMachine::Callbacks ModbusGatewayWorker::makePulseCallbacks(ModbusGatewayWorker *w)
{
    PulseStateMachine::Callbacks cb;
    // writeCoil: route through the request queue. A rejected enqueue (queue
    // closed = offline) returns false and aborts the pulse (spec §8.4
    // no-replay). submissionId stays 0: the pulse's own completion is emitted
    // by the finished callback under the submission identity allocated by
    // submitPulse().
    cb.writeCoil = [w](quint16 address, bool value, CommandPriority priority) {
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::WriteCoil;
        req.address = address;
        req.value = value ? 1 : 0;
        req.writeThenReadback = true; // confirm by readback (spec §8.4)
        req.cls = requestClassFor(priority);
        if (!w->m_queue.enqueue(req))
            return false;
        w->tryDispatch();
        return true;
    };
    // readCoil: dedicated single-coil pulse readback (spec §8.5).
    cb.readCoil = [w](quint16 address) {
        w->enqueuePulseReadback(address);
        return true;
    };
    // finished: exactly one terminal completion for the pulse submission.
    cb.finished = [w](quint16 address, bool ok, quint64 submissionId) {
        w->emitCompletion(submissionId, w->m_gatewayGeneration, PlcOperation::Pulse,
                          address, ok,
                          ok ? QString()
                             : QStringLiteral("pulse not confirmed by readback"));
    };
    return cb;
}

ModbusGatewayWorker::ModbusGatewayWorker(const QtModbusPlcGateway::Config &cfg,
                                         IModbusTransport *transport, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_transport(transport)
    , m_ownsTransport(transport == nullptr)
    , m_pulses(makePulseCallbacks(this), [this]() { return m_nowMs(); })
    , m_nowMs([]() { return QDateTime::currentMSecsSinceEpoch(); })
{
    registerPlcGatewayMetaTypes();

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(50);
    connect(m_pollTimer, &QTimer::timeout, this, &ModbusGatewayWorker::onPollTick);

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &ModbusGatewayWorker::onReconnectTick);

    // Injected transports (tests) are connected here; owned transports are
    // connected in start() when they are created.
    if (m_transport) {
        connect(m_transport, &IModbusTransport::transferFinished, this,
                &ModbusGatewayWorker::onTransferFinished);
    }
}

ModbusGatewayWorker::~ModbusGatewayWorker()
{
    if (m_ownsTransport)
        delete m_transport;
}

bool ModbusGatewayWorker::isOnline() const
{
    return m_state == LinkState::Online && m_hasValidSnapshot;
}

void ModbusGatewayWorker::setNowMs(std::function<qint64()> now)
{
    m_nowMs = std::move(now);
}

void ModbusGatewayWorker::setPollIntervals(int fastMs, int homeMs, int commandMs, int slowMs)
{
    m_fastMs = fastMs;
    m_homeMs = homeMs;
    m_commandMs = commandMs;
    m_slowMs = slowMs;
}

void ModbusGatewayWorker::setGatewayGeneration(quint64 generation)
{
    m_gatewayGeneration = generation;
}

void ModbusGatewayWorker::start()
{
    if (m_started)
        return; // double start() would leak the old transport
    m_started = true;
    if (m_ownsTransport) {
        m_transport = new RtuTransport(m_cfg, this);
        connect(m_transport, &IModbusTransport::transferFinished, this,
                &ModbusGatewayWorker::onTransferFinished);
    }
    m_policy = ReconnectPolicy();
    m_state = LinkState::Disconnected;
    m_busy = false;
    m_hasValidSnapshot = false;
    m_sequence = 0;
    m_data = DeviceSnapshotData();
    m_fastEvidence = BlockEvidence::NoEvidence;
    m_homeEvidence = BlockEvidence::NoEvidence;
    m_commandEvidence = BlockEvidence::NoEvidence;
    m_slowEvidence = BlockEvidence::NoEvidence;
    m_pendingConfirmations.clear();
    m_haveHeartbeat = false;
    m_inFlight = std::nullopt;

    openLink();
    m_pollTimer->start();
}

void ModbusGatewayWorker::stop()
{
    m_pollTimer->stop();
    m_reconnectTimer->stop();
    // Abort any active pulses (spec §8.5: stop aborts with finished(false),
    // nothing queued).
    m_pulses.reset();
    m_pulseReadbacks.clear();
    if (m_transport)
        m_transport->close();
    // Delete an owned transport so a stop()->start() reconfiguration does not
    // accumulate dead QObjects and stale transferFinished connections (the
    // injected test transport is parented by the test and must survive).
    if (m_ownsTransport && m_transport) {
        delete m_transport;
        m_transport = nullptr;
    }
    m_state = LinkState::Disconnected;
    m_queue.close();
    // Report every accepted-but-unfinished write as failed — never silently
    // dropped (contract IPlcGateway lifecycle: converge on shutdown).
    reportDroppedWrites(QStringLiteral("gateway stopped"));
    m_queue.clear();
    m_inFlight = std::nullopt;
    // Reset the start guard and busy flag so a stop->start restart actually
    // re-creates/reopens the transport and runs again.
    m_started = false;
    m_busy = false;
    // Mirror the offline state so isOnline()/connectionStateChanged() do not
    // stay stale after stop (same signal enterOffline() emits).
    emit connectionStateChanged(m_gatewayGeneration, false);
}

SubmissionResult ModbusGatewayWorker::submitWriteCoil(quint16 address, bool value,
                                                      CommandPriority priority)
{
    SubmissionResult result;
    result.gateway_generation = m_gatewayGeneration;

    ModbusRequest req;
    req.kind = ModbusRequest::Kind::WriteCoil;
    req.address = address;
    req.value = value ? 1 : 0;
    req.writeThenReadback = true; // spec §8.4: confirm writes by readback
    req.cls = requestClassFor(priority);
    req.submissionId = m_nextRequestId++;
    req.operation = PlcOperation::WriteCoil;
    req.gatewayGeneration = m_gatewayGeneration;

    if (!m_queue.enqueue(req)) {
        result.accepted = false;
        result.request_id = 0;
        result.immediate_rejection_reason =
            QStringLiteral("offline: command rejected, not replayed");
        return result;
    }
    result.accepted = true;
    result.request_id = req.submissionId;
    tryDispatch();
    return result;
}

SubmissionResult ModbusGatewayWorker::submitWriteRegister(quint16 address, quint16 value,
                                                          CommandPriority priority)
{
    SubmissionResult result;
    result.gateway_generation = m_gatewayGeneration;

    ModbusRequest req;
    req.kind = ModbusRequest::Kind::WriteRegister;
    req.address = address;
    req.value = value;
    req.writeThenReadback = true;
    req.cls = requestClassFor(priority);
    req.submissionId = m_nextRequestId++;
    req.operation = PlcOperation::WriteRegister;
    req.gatewayGeneration = m_gatewayGeneration;

    if (!m_queue.enqueue(req)) {
        result.accepted = false;
        result.request_id = 0;
        result.immediate_rejection_reason =
            QStringLiteral("offline: command rejected, not replayed");
        return result;
    }
    result.accepted = true;
    result.request_id = req.submissionId;
    tryDispatch();
    return result;
}

SubmissionResult ModbusGatewayWorker::submitPulse(quint16 address)
{
    SubmissionResult result;
    result.gateway_generation = m_gatewayGeneration;

    const quint64 requestId = m_nextRequestId++;
    if (!m_pulses.startPulse(address, requestId)) {
        // Offline, or a pulse on the same address is already active: rejected
        // without a completion (contract submission rule).
        result.accepted = false;
        result.request_id = 0;
        result.immediate_rejection_reason = m_queue.isClosed()
            ? QStringLiteral("offline: pulse rejected, not replayed")
            : QStringLiteral("a pulse on this address is already active");
        return result;
    }
    result.accepted = true;
    result.request_id = requestId;
    return result;
}

void ModbusGatewayWorker::onPollTick()
{
    // Drive the pulse hold timer from the 50 ms poll tick (spec §7.2, §8.5).
    m_pulses.onTick();
    // Expire readback confirmations whose defensive timeout elapsed
    // (PLC-HMI-003 D2: parameter writes converge even without a readback).
    expireWriteConfirmations();

    if (m_state != LinkState::Online)
        return;

    const qint64 now = m_nowMs();
    // A block without a successful transfer inside its approved threshold
    // becomes observable as Stale from the poll tick alone — no success and no
    // transfer failure is required (contract D6 quality rule).
    publishStaleTransitions();
    if (now - m_lastFastMs >= m_fastMs) {
        m_lastFastMs = now;
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::ReadRegisters;
        req.address = kFastStart;
        req.count = kFastCount;
        req.cls = RequestClass::FastPoll;
        req.retriesLeft = m_cfg.readRetries;
        m_queue.enqueuePoll(req);
    }
    if (now - m_lastHomeMs >= m_homeMs) {
        m_lastHomeMs = now;
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::ReadCoils;
        req.address = kHomeStart;
        req.count = kHomeCount;
        req.cls = RequestClass::HomePoll;
        req.retriesLeft = m_cfg.readRetries;
        m_queue.enqueuePoll(req);
    }
    if (now - m_lastCommandMs >= m_commandMs) {
        m_lastCommandMs = now;
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::ReadCoils;
        req.address = kCommandStart;
        req.count = kCommandCount;
        req.cls = RequestClass::CommandPoll;
        req.retriesLeft = m_cfg.readRetries;
        m_queue.enqueuePoll(req);
    }
    if (now - m_lastSlowMs >= m_slowMs) {
        m_lastSlowMs = now;
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::ReadRegisters;
        req.address = kSlowStart;
        req.count = kSlowCount;
        req.cls = RequestClass::SlowPoll;
        req.retriesLeft = m_cfg.readRetries;
        m_queue.enqueuePoll(req);
    }

    tryDispatch();
}

void ModbusGatewayWorker::onReconnectTick()
{
    if (m_state != LinkState::Offline)
        return;
    m_policy.onReconnectAttempted();
    openLink();
}

void ModbusGatewayWorker::openLink()
{
    if (!m_transport)
        return;
    // A reconnect/replacement is a new gateway generation: pending
    // submissions of the old generation have already been converged, and new
    // submissions carry the new generation.
    if (m_hasOpened)
        ++m_gatewayGeneration;
    m_hasOpened = true;
    if (m_gatewayGeneration == 0)
        m_gatewayGeneration = 1;

    m_state = LinkState::Connecting;
    if (!m_transport->open()) {
        enterOffline();
        return;
    }
    m_state = LinkState::Online;
    // Keep the queue closed until the first full valid snapshot arrives after
    // (re)connect: non-safety control must not resume before a full snapshot
    // (spec §8.4). Writes submitted in this window are rejected, not queued.
    m_queue.close();
    m_queue.clear(); // no replay of anything queued while offline (spec §8.4)
    m_hasValidSnapshot = false;
    m_haveHeartbeat = false;
    m_lastFastMs = 0;
    m_lastHomeMs = 0;
    m_lastCommandMs = 0;
    m_lastSlowMs = 0;
    // A (re)opened link is a new session: no block has evidence yet, and its
    // no-success age starts here so it can become Stale without ever
    // succeeding (contract D6 quality rule).
    const qint64 openedAtMs = m_nowMs();
    m_lastFastSuccessMs = openedAtMs;
    m_lastHomeSuccessMs = openedAtMs;
    m_lastCommandSuccessMs = openedAtMs;
    m_lastSlowSuccessMs = openedAtMs;
    m_fastEvidence = BlockEvidence::NoEvidence;
    m_homeEvidence = BlockEvidence::NoEvidence;
    m_commandEvidence = BlockEvidence::NoEvidence;
    m_slowEvidence = BlockEvidence::NoEvidence;
    m_pendingConfirmations.clear();
    emit connectionStateChanged(m_gatewayGeneration, false); // not fully online yet

    // Fetch a full fast block immediately so the first snapshot arrives
    // promptly after (re)connect (spec §8.4). Polls are allowed even while
    // the queue is closed (writes are not).
    ModbusRequest fast;
    fast.kind = ModbusRequest::Kind::ReadRegisters;
    fast.address = kFastStart;
    fast.count = kFastCount;
    fast.cls = RequestClass::FastPoll;
    fast.retriesLeft = m_cfg.readRetries;
    m_queue.enqueuePoll(fast);

    tryDispatch();
}

void ModbusGatewayWorker::enterOffline()
{
    m_state = LinkState::Offline;
    m_queue.close();
    m_hasValidSnapshot = false;
    m_busy = false;
    // Abort any active pulses (spec §8.5: offline aborts with finished(false),
    // nothing queued).
    m_pulses.reset();
    m_pulseReadbacks.clear();
    // Report every in-flight, ack'd-but-unconfirmed and queued write as failed
    // — never silently dropped (contract IPlcGateway lifecycle).
    reportDroppedWrites(QStringLiteral("offline"));
    m_queue.clear();
    emit connectionStateChanged(m_gatewayGeneration, false);
    scheduleReconnect();
}

void ModbusGatewayWorker::emitCompletion(quint64 submissionId, quint64 generation,
                                         PlcOperation operation, quint16 address,
                                         bool ok, const QString &error)
{
    if (submissionId == 0)
        return;
    SubmissionCompletion completion;
    completion.request_id = submissionId;
    completion.gateway_generation = generation;
    completion.operation = operation;
    completion.address = address;
    completion.result = ok;
    completion.error = error;
    emit submissionCompleted(completion);
}

void ModbusGatewayWorker::reportDroppedWrites(const QString &reason)
{
    // An in-flight write must still report its result; otherwise the consumer
    // waits forever (contract IPlcGateway lifecycle: converge on stop/offline).
    if (m_inFlight
        && (m_inFlight->kind == ModbusRequest::Kind::WriteCoil
            || m_inFlight->kind == ModbusRequest::Kind::WriteRegister)) {
        if (m_inFlight->submissionId != 0) {
            emitCompletion(m_inFlight->submissionId, m_inFlight->gatewayGeneration,
                           m_inFlight->operation, m_inFlight->address, false, reason);
            // The write moved into the confirmation table only after its ack;
            // erase a later duplicate emission for the same identity.
            m_pendingConfirmations.remove(m_inFlight->id);
        }
    }
    m_inFlight = std::nullopt;
    // A write ack'd but not yet confirmed by readback, and a write queued but
    // not yet dispatched, must both report failure — never silently dropped.
    for (auto it = m_pendingConfirmations.constBegin();
         it != m_pendingConfirmations.constEnd(); ++it) {
        emitCompletion(it->submissionId, it->gatewayGeneration, it->operation,
                       it->address, false, reason);
    }
    m_pendingConfirmations.clear();
    ModbusRequest queued;
    while (m_queue.next(queued)) {
        if (queued.kind == ModbusRequest::Kind::WriteCoil
            || queued.kind == ModbusRequest::Kind::WriteRegister) {
            if (queued.submissionId != 0) {
                emitCompletion(queued.submissionId, queued.gatewayGeneration,
                               queued.operation, queued.address, false, reason);
            }
        }
    }
}

void ModbusGatewayWorker::scheduleReconnect()
{
    ++m_reconnectCount; // comm stats (spec §16)
    m_reconnectTimer->start(m_policy.nextReconnectDelayMs());
}

void ModbusGatewayWorker::tryDispatch()
{
    if (m_busy || m_state != LinkState::Online)
        return;

    ModbusRequest req;
    if (!m_queue.next(req))
        return;

    m_inFlight = req;
    m_busy = true;

    if (!m_transport->send(req)) {
        // Link dropped mid-flight; treat as a transfer failure.
        m_policy.onTransferFailure();
        if (m_policy.isOffline()) {
            enterOffline();
            return;
        }
        // Not yet offline (1st/2nd failure): report the write directly so it
        // is not silently dropped.
        if (req.kind == ModbusRequest::Kind::WriteCoil
            || req.kind == ModbusRequest::Kind::WriteRegister) {
            m_pulses.onWriteCompleted(req.address, false); // uncertain write (spec §8.5)
            if (req.submissionId != 0) {
                emitCompletion(req.submissionId, req.gatewayGeneration,
                               req.operation, req.address, false,
                               QStringLiteral("send failed"));
            }
        } else if (req.isReadback) {
            // A readback that could not be sent can never confirm the write:
            // fail it and drop the pending confirmation so it never leaks.
            const auto it = m_pendingConfirmations.constFind(req.requestId);
            if (it != m_pendingConfirmations.constEnd()) {
                emitCompletion(it->submissionId, it->gatewayGeneration,
                               it->operation, it->address, false,
                               QStringLiteral("readback send failed"));
                m_pendingConfirmations.erase(it);
            } else if (m_pulseReadbacks.contains(req.requestId)) {
                // Pulse readback: converge the pulse as bit 0 (defined
                // outcome, spec §8.4) and drop the entry.
                const quint16 addr = m_pulseReadbacks.take(req.requestId);
                m_pulses.onReadback(addr, false);
            }
        }
        m_inFlight = std::nullopt;
        m_busy = false;
        return;
    }
    if (req.cls == RequestClass::FastPoll)
        m_fastDispatchedMs = m_nowMs(); // for dataAgeMs of the next snapshot
}

void ModbusGatewayWorker::onTransferFinished(const TransferResult &res)
{
    if (!m_busy || !m_inFlight)
        return;
    const ModbusRequest req = *m_inFlight;
    m_inFlight = std::nullopt;
    m_busy = false;

    if (!res.ok) {
        if (req.kind == ModbusRequest::Kind::ReadCoils
            || req.kind == ModbusRequest::Kind::ReadRegisters) {
            if (req.retriesLeft > 0) {
                ModbusRequest retry = req;
                --retry.retriesLeft;
                if (m_transport->send(retry)) {
                    m_inFlight = retry;
                    m_busy = true;
                    return;
                }
            }
            ++m_failedPolls; // comm stats (spec §16)
            // Mark only the failed source block ProtocolError; it stays
            // non-valid until a successful refresh (contract invariant 461).
            bool qualityDowngraded = false;
            switch (req.cls) {
            case RequestClass::FastPoll:
                m_fastEvidence = BlockEvidence::Failed;
                qualityDowngraded = true;
                break;
            case RequestClass::HomePoll:
                m_homeEvidence = BlockEvidence::Failed;
                qualityDowngraded = true;
                break;
            case RequestClass::CommandPoll:
                m_commandEvidence = BlockEvidence::Failed;
                qualityDowngraded = true;
                break;
            case RequestClass::SlowPoll:
                m_slowEvidence = BlockEvidence::Failed;
                qualityDowngraded = true;
                break;
            default:
                break;
            }
            // Publish the degraded quality so consumers see the failure now.
            if (qualityDowngraded && m_hasValidSnapshot)
                publishSnapshot();
        }
        if (req.kind == ModbusRequest::Kind::WriteCoil
            || req.kind == ModbusRequest::Kind::WriteRegister) {
            // A failed write must still report its result (spec §8.4).
            m_pulses.onWriteCompleted(req.address, false); // uncertain write (spec §8.5)
            if (req.submissionId != 0) {
                emitCompletion(req.submissionId, req.gatewayGeneration,
                               req.operation, req.address, false, res.error);
            }
        } else if (req.isReadback) {
            // A failed readback can never confirm the write: fail it and
            // drop the pending confirmation so it never leaks.
            const auto it = m_pendingConfirmations.constFind(req.requestId);
            if (it != m_pendingConfirmations.constEnd()) {
                emitCompletion(it->submissionId, it->gatewayGeneration,
                               it->operation, it->address, false,
                               QStringLiteral("readback transfer failed"));
                m_pendingConfirmations.erase(it);
            } else if (m_pulseReadbacks.contains(req.requestId)) {
                // Pulse readback: converge the pulse as bit 0 (defined
                // outcome, spec §8.4) and drop the entry.
                const quint16 addr = m_pulseReadbacks.take(req.requestId);
                m_pulses.onReadback(addr, false);
            }
        }
        m_policy.onTransferFailure();
        if (m_policy.isOffline()) {
            enterOffline();
            return;
        }
        tryDispatch();
        return;
    }

    m_policy.onTransferSuccess();

    switch (req.kind) {
    case ModbusRequest::Kind::ReadRegisters:
        handleReadResult(req, res);
        break;
    case ModbusRequest::Kind::ReadCoils:
        handleReadResult(req, res);
        break;
    case ModbusRequest::Kind::WriteCoil:
    case ModbusRequest::Kind::WriteRegister:
        handleWriteResult(req, res);
        break;
    }

    tryDispatch();
}

void ModbusGatewayWorker::handleReadResult(const ModbusRequest &req, const TransferResult &res)
{
    // Dedicated pulse readback (spec §8.5): a single-coil read requested by
    // the pulse state machine, matched by request identity. It is NOT routed
    // through the write-confirmation table (isReadback is consumed there).
    if (req.isReadback && m_pulseReadbacks.contains(req.requestId)) {
        handlePulseReadback(req, res);
        return;
    }

    // Readback of a previously written value (spec §8.4) takes precedence:
    // a readback is a single-register/coil read matched by request identity
    // (isReadback + requestId), never by address alone — a poll whose start
    // address equals a pending write's address must not be consumed as the
    // readback.
    if (req.isReadback) {
        const auto it = m_pendingConfirmations.constFind(req.requestId);
        if (it != m_pendingConfirmations.constEnd()) {
            const PendingWrite pw = it.value();
            m_pendingConfirmations.erase(it);

            bool confirmed = false;
            if (req.kind == ModbusRequest::Kind::ReadCoils) {
                // Single-coil readback: the coil at req.address is bit 0 of
                // the first value.
                const quint16 bits = res.values.isEmpty() ? 0 : res.values.first();
                confirmed = ((bits & 0x0001) ? 1 : 0) == pw.expected;
            } else {
                confirmed = !res.values.isEmpty() && res.values.first() == pw.expected;
            }

            if (confirmed) {
                emitCompletion(pw.submissionId, pw.gatewayGeneration, pw.operation,
                               pw.address, true, QString());
            } else if (pw.retriesLeft > 0) {
                // Target not yet effective: retry per policy (spec §8.4).
                PendingWrite next = pw;
                --next.retriesLeft;
                m_pendingConfirmations.insert(req.requestId, next);
                ModbusRequest readback = req;
                readback.retriesLeft = 0;
                m_queue.enqueue(readback);
            } else {
                emitCompletion(pw.submissionId, pw.gatewayGeneration, pw.operation,
                               pw.address, false,
                               QStringLiteral("write not confirmed by readback"));
            }
        }
        return;
    }

    const qint64 now = m_nowMs();
    if (req.cls == RequestClass::FastPoll) {
        if (res.values.size() >= kFastCount) {
            quint16 raw[41];
            for (int i = 0; i < kFastCount; ++i)
                raw[i] = res.values.at(i);
            const QDateTime started = QDateTime::currentDateTime();
            const qint64 ageMs = qMax<qint64>(0, m_nowMs() - m_fastDispatchedMs);
            // The fast block decode builds a fresh DeviceSnapshotData. The
            // slow-block fields (D204/D210/D220) and the home/command readback
            // bits are filled by their own polls, which do NOT rebuild the
            // fast block; they must survive the fast poll, or the published
            // snapshot would always show them as 0 (breaking M50-M53/M100-M111
            // and the M100 estop branch). Block quality/ages are recomputed from
            // the per-block evidence in publishSnapshot().
            const quint16 pulsePerMm = m_data.pulsePerMm;
            const qint16 widthDelta = m_data.widthDelta;
            const quint16 widthSpeed = m_data.widthSpeed;
            const quint32 slowInvalid = m_data.invalidFields
                & ((quint32(1) << quint8(SnapshotField::PulsePerMm))
                   | (quint32(1) << quint8(SnapshotField::WidthSpeed)));
            const quint16 homeBits = m_data.homeBits;
            const quint16 commandBits = m_data.commandBits;
            m_data = decodeFastBlock(raw, 0, true, ageMs, started, started,
                                     DataQuality::Valid);
            m_data.pulsePerMm = pulsePerMm;
            m_data.widthDelta = widthDelta;
            m_data.widthSpeed = widthSpeed;
            m_data.homeBits = homeBits;
            m_data.commandBits = commandBits;
            m_data.invalidFields |= slowInvalid;
            m_fastEvidence = BlockEvidence::Valid;
            m_lastFastSuccessMs = now;
            checkHeartbeatFreeze(m_data.heartbeat);
            if (m_state != LinkState::Online)
                return; // heartbeat freeze took us offline
            const bool wasOnline = m_hasValidSnapshot;
            m_hasValidSnapshot = true;
            publishSnapshot();
            if (!wasOnline) {
                // First full valid snapshot after (re)connect: the link is
                // fully online again — reset the reconnect schedule and the
                // consecutive-failure counter (spec §8.4).
                m_policy.onReconnectSucceeded();
                // Reopen the queue: non-safety control may resume now that a
                // full valid snapshot is in hand (spec §8.4).
                m_queue.reopen();
                emit connectionStateChanged(m_gatewayGeneration, true);
            }
        }
        return;
    }

    // Non-fast polls update the accumulated data and their own block evidence
    // in place; the next fast poll publishes the recomputed quality/ages (no
    // extra snapshot per block — mirrors the original publication cadence).
    if (req.cls == RequestClass::HomePoll) {
        if (!res.values.isEmpty())
            m_data.homeBits = res.values.first();
        m_homeEvidence = BlockEvidence::Valid;
        m_lastHomeSuccessMs = now;
        return;
    }

    if (req.cls == RequestClass::CommandPoll) {
        if (!res.values.isEmpty())
            m_data.commandBits = res.values.first();
        m_commandEvidence = BlockEvidence::Valid;
        m_lastCommandSuccessMs = now;
        return;
    }

    if (req.cls == RequestClass::SlowPoll) {
        if (res.values.size() >= 20) {
            m_data.pulsePerMm = res.values.at(0);   // D204
            m_data.widthDelta = decode::i16(res.values.at(6));  // D210
            m_data.widthSpeed = res.values.at(16);  // D220
            // Out-of-range D204/D220 mark the field invalid (spec §9) and
            // D210 gets its own validity metadata.
            checkSlowBlockRange(m_data);
        }
        m_slowEvidence = BlockEvidence::Valid;
        m_lastSlowSuccessMs = now;
        return;
    }
}

void ModbusGatewayWorker::handleWriteResult(const ModbusRequest &req, const TransferResult &res)
{
    Q_UNUSED(res);
    // Feed the pulse state machine (spec §8.5): the write-1 ack starts the
    // hold timing, the write-0 ack completes the pulse. The machine ignores
    // addresses with no active pulse.
    m_pulses.onWriteCompleted(req.address, true);
    if (!req.writeThenReadback) {
        if (req.submissionId != 0) {
            emitCompletion(req.submissionId, req.gatewayGeneration, req.operation,
                           req.address, true, QString());
        }
        return;
    }
    // Write acknowledged: confirm by readback (spec §8.4).
    PendingWrite pw;
    pw.address = req.address;
    pw.expected = req.value;
    pw.operation = req.operation;
    pw.submissionId = req.submissionId;
    pw.gatewayGeneration = req.gatewayGeneration;
    pw.retriesLeft = m_cfg.readRetries;
    pw.deadlineMs = m_nowMs() + m_writeConfirmTimeoutMs;
    m_pendingConfirmations.insert(req.id, pw);

    ModbusRequest readback;
    readback.kind = (req.kind == ModbusRequest::Kind::WriteCoil)
        ? ModbusRequest::Kind::ReadCoils
        : ModbusRequest::Kind::ReadRegisters;
    readback.address = req.address;
    readback.count = 1;
    readback.cls = RequestClass::UserWrite; // write-then-readback (spec §8.3)
    readback.isReadback = true;             // matched by request identity
    readback.requestId = req.id;
    if (!m_queue.enqueue(readback)) {
        // The confirmation can never arrive while offline: converge now.
        const auto it = m_pendingConfirmations.constFind(req.id);
        if (it != m_pendingConfirmations.constEnd()) {
            emitCompletion(it->submissionId, it->gatewayGeneration, it->operation,
                           it->address, false,
                           QStringLiteral("offline: readback not queued"));
            m_pendingConfirmations.erase(it);
        }
    }
}

void ModbusGatewayWorker::enqueuePulseReadback(quint16 address)
{
    // Dedicated single-coil readback for the pulse state machine (spec §8.5).
    // Matched by request identity (isReadback + requestId) but kept separate
    // from the write-confirmation table: the pulse machine's readback is a
    // convergence probe, not a write confirmation.
    ModbusRequest readback;
    readback.kind = ModbusRequest::Kind::ReadCoils;
    readback.address = address;
    readback.count = 1;
    readback.cls = RequestClass::UserWrite;
    readback.isReadback = true;
    readback.requestId = m_pulseReadbackId++;
    m_pulseReadbacks.insert(readback.requestId, address);
    if (!m_queue.enqueue(readback)) {
        // Offline: the readback can never arrive; abort the pulse.
        m_pulseReadbacks.remove(readback.requestId);
        m_pulses.onReadback(address, false);
    }
    tryDispatch();
}

void ModbusGatewayWorker::handlePulseReadback(const ModbusRequest &req,
                                             const TransferResult &res)
{
    const quint16 address = m_pulseReadbacks.take(req.requestId);
    if (!res.ok) {
        // A failed readback cannot converge the pulse: treat as bit 0 (the
        // machine then finishes failed for an uncertain set, or completes for
        // an uncertain clear — both defined outcomes, spec §8.4).
        m_pulses.onReadback(address, false);
        return;
    }
    const quint16 bits = res.values.isEmpty() ? 0 : res.values.first();
    m_pulses.onReadback(address, (bits & 0x0001) ? true : false);
}

void ModbusGatewayWorker::expireWriteConfirmations()
{
    if (m_pendingConfirmations.isEmpty())
        return;
    const qint64 now = m_nowMs();
    const auto ids = m_pendingConfirmations.keys();
    for (quint64 id : ids) {
        const auto it = m_pendingConfirmations.constFind(id);
        if (it == m_pendingConfirmations.constEnd())
            continue;
        if (it->deadlineMs <= 0 || now < it->deadlineMs)
            continue;
        const PendingWrite pw = it.value();
        m_pendingConfirmations.erase(it);
        emitCompletion(pw.submissionId, pw.gatewayGeneration, pw.operation,
                       pw.address, false,
                       QStringLiteral("write confirmation timeout"));
    }
}

DataQuality ModbusGatewayWorker::evidenceQuality(BlockEvidence evidence, qint64 ageMs,
                                                 qint64 staleThresholdMs)
{
    if (evidence == BlockEvidence::Failed) {
        // A failed transfer stays ProtocolError until a successful refresh
        // (contract invariant 461), regardless of the block's age.
        return DataQuality::ProtocolError;
    }
    if (ageMs > staleThresholdMs)
        return DataQuality::Stale;
    return evidence == BlockEvidence::Valid ? DataQuality::Valid
                                            : DataQuality::ProtocolError;
}

void ModbusGatewayWorker::refreshBlockEvidence(qint64 now)
{
    // Real per-block ages from monotonic timestamps; the timestamps start at
    // the session time, so a block that never succeeds still ages and becomes
    // Stale past its approved threshold.
    m_data.fast_age_ms = qMax<qint64>(0, now - m_lastFastSuccessMs);
    m_data.home_age_ms = qMax<qint64>(0, now - m_lastHomeSuccessMs);
    m_data.command_age_ms = qMax<qint64>(0, now - m_lastCommandSuccessMs);
    m_data.slow_age_ms = qMax<qint64>(0, now - m_lastSlowSuccessMs);
    m_data.fast_quality = evidenceQuality(m_fastEvidence, m_data.fast_age_ms, kFastStaleMs);
    m_data.home_quality = evidenceQuality(m_homeEvidence, m_data.home_age_ms, kHomeStaleMs);
    m_data.command_quality =
        evidenceQuality(m_commandEvidence, m_data.command_age_ms, kCommandStaleMs);
    m_data.slow_quality = evidenceQuality(m_slowEvidence, m_data.slow_age_ms, kSlowStaleMs);
    recomputeDerivedQuality(m_data);
}

void ModbusGatewayWorker::publishStaleTransitions()
{
    if (!m_hasValidSnapshot)
        return;
    refreshBlockEvidence(m_nowMs());
    // Publish once per block transition into Stale. Recovery to Valid is
    // carried by the next successful-transfer publication, so unchanged
    // qualities are never re-published.
    const bool crossedFast = m_data.fast_quality == DataQuality::Stale
        && m_publishedFastQuality != DataQuality::Stale;
    const bool crossedHome = m_data.home_quality == DataQuality::Stale
        && m_publishedHomeQuality != DataQuality::Stale;
    const bool crossedCommand = m_data.command_quality == DataQuality::Stale
        && m_publishedCommandQuality != DataQuality::Stale;
    const bool crossedSlow = m_data.slow_quality == DataQuality::Stale
        && m_publishedSlowQuality != DataQuality::Stale;
    if (crossedFast || crossedHome || crossedCommand || crossedSlow)
        publishSnapshot();
}

void ModbusGatewayWorker::publishSnapshot()
{
    const qint64 now = m_nowMs();
    // Evidence-based quality and real per-block ages (contract invariant 461):
    // a failed transfer stays ProtocolError until a successful refresh, and a
    // block without success inside its approved stale threshold becomes Stale.
    refreshBlockEvidence(now);
    m_publishedFastQuality = m_data.fast_quality;
    m_publishedHomeQuality = m_data.home_quality;
    m_publishedCommandQuality = m_data.command_quality;
    m_publishedSlowQuality = m_data.slow_quality;

    m_data.connected = true;
    m_data.sequence = ++m_sequence;
    m_data.captureCompleted = QDateTime::currentDateTime();
    const DeviceSnapshot snapshot(m_data);
    emit snapshotReady(m_gatewayGeneration, snapshot);
    // Communication statistics ride along with every published snapshot
    // (spec §16): the counters belong to the snapshot's sequence and are
    // emitted by the port itself (no concrete cast needed).
    PlcCommStats stats;
    stats.gateway_generation = m_gatewayGeneration;
    stats.snapshot_sequence = m_sequence;
    stats.per_block_age_ms = snapshot.overall_age_ms;
    stats.reconnect_count = m_reconnectCount;
    stats.failed_polls = m_failedPolls;
    emit commStatsChanged(stats);
}

void ModbusGatewayWorker::checkHeartbeatFreeze(quint16 heartbeat)
{
    const qint64 now = m_nowMs();
    if (!m_haveHeartbeat) {
        m_haveHeartbeat = true;
        m_lastHeartbeat = heartbeat;
        m_lastHeartbeatChangeMs = now;
        return;
    }
    if (decode::heartbeatActive(m_lastHeartbeat, heartbeat)) {
        m_lastHeartbeat = heartbeat;
        m_lastHeartbeatChangeMs = now;
        return;
    }
    if (now - m_lastHeartbeatChangeMs >= kHeartbeatFreezeMs) {
        m_policy.onHeartbeatFreeze();
        enterOffline();
    }
}

} // namespace hlm

#include "adapters/modbus/qt_modbus_plc_gateway.moc"
