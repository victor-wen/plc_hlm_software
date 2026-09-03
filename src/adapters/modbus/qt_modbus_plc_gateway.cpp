#include "adapters/modbus/qt_modbus_plc_gateway.h"

#include <QDateTime>
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
constexpr quint16 kCommandCount = 13;  // M100-M112
constexpr quint16 kSlowStart = 204;    // D204
constexpr quint16 kSlowCount = 20;     // D204-D223

// D140 heartbeat freeze threshold (spec §8.4).
constexpr qint64 kHeartbeatFreezeMs = 3000;

// Read requests may retry once (spec §8.4).
constexpr int kReadRetries = 1;

} // namespace

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

        connect(reply, &QModbusReply::finished, this, [this, reply]() {
            TransferResult res;
            if (reply->error() == QModbusDevice::NoError) {
                res.ok = true;
                const QModbusDataUnit result = reply->result();
                for (int i = 0; i < result.values().size(); ++i)
                    res.values.append(quint16(result.values().at(i)));
            } else {
                res.ok = false;
                res.error = reply->errorString();
            }
            reply->deleteLater();
            emit transferFinished(res);
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
    // DeviceSnapshot crosses the worker->facade thread boundary via a queued
    // connection; the metatype must be registered or Qt drops every snapshot.
    qRegisterMetaType<hlm::DeviceSnapshot>();
    // CommandPriority is marshalled via Q_ARG in queued invokeMethod calls
    // (writeCoil/writeRegister); register it so the queued call is not dropped.
    qRegisterMetaType<hlm::CommandPriority>();

    m_worker = new ModbusGatewayWorker(m_cfg, nullptr, nullptr);
    m_worker->moveToThread(&m_thread);

    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &ModbusGatewayWorker::snapshotReady, this,
            &QtModbusPlcGateway::snapshotReady);
    connect(m_worker, &ModbusGatewayWorker::connectionStateChanged, this,
            [this](bool online) {
                m_online = online;
                emit connectionStateChanged(online);
            });
    connect(m_worker, &ModbusGatewayWorker::writeCompleted, this,
            &QtModbusPlcGateway::writeCompleted);
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
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

void QtModbusPlcGateway::stop()
{
    if (m_thread.isRunning())
        QMetaObject::invokeMethod(m_worker, "stop", Qt::BlockingQueuedConnection);
    // Mirror the offline state so isOnline()/connectionStateChanged() do not
    // stay stale after stop (the worker emits nothing on stop).
    if (m_online) {
        m_online = false;
        emit connectionStateChanged(false);
    }
    // Reset the start guard so a stop->start restart actually runs again.
    m_started = false;
}

bool QtModbusPlcGateway::isOnline() const
{
    return m_online;
}

void QtModbusPlcGateway::writeCoil(quint16 address, bool value, CommandPriority priority)
{
    QMetaObject::invokeMethod(m_worker, "submitWriteCoil", Qt::QueuedConnection,
                              Q_ARG(quint16, address), Q_ARG(bool, value),
                              Q_ARG(CommandPriority, priority));
}

void QtModbusPlcGateway::writeRegister(quint16 address, quint16 value, CommandPriority priority)
{
    QMetaObject::invokeMethod(m_worker, "submitWriteRegister", Qt::QueuedConnection,
                              Q_ARG(quint16, address), Q_ARG(quint16, value),
                              Q_ARG(CommandPriority, priority));
}

bool QtModbusPlcGateway::startPulse(quint16 address)
{
    // BlockingQueued: the caller (UI thread) needs the synchronous accept/
    // reject answer (spec §8.5: startPulse returns whether the pulse started).
    bool ok = false;
    QMetaObject::invokeMethod(m_worker, "startPulse", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, ok), Q_ARG(quint16, address));
    return ok;
}

// ---------------------------------------------------------------------------
// ModbusGatewayWorker
// ---------------------------------------------------------------------------
namespace {

// Map a CommandPriority onto the request queue class (spec §8.3).
RequestClass requestClassFor(CommandPriority priority)
{
    return (priority == CommandPriority::Safety) ? RequestClass::SafetyWrite
         : (priority == CommandPriority::PulseClear) ? RequestClass::PulseClear
         : (priority == CommandPriority::Heartbeat) ? RequestClass::Heartbeat
                                                    : RequestClass::UserWrite;
}

} // namespace

PulseStateMachine::Callbacks ModbusGatewayWorker::makePulseCallbacks(ModbusGatewayWorker *w)
{
    PulseStateMachine::Callbacks cb;
    // writeCoil: route through the request queue. A rejected enqueue (queue
    // closed = offline) returns false and aborts the pulse (spec §8.4
    // no-replay).
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
    // finished: no-op by design (spec §8.5 design decision). The pulse
    // outcome is presented to the UI via writeCompleted: the write-0 readback
    // confirmation reports success, the failed-write path reports failure,
    // and the coordinator converges on the snapshot.
    cb.finished = [](quint16, bool) {};
    return cb;
}

WatchdogTimer::Callbacks ModbusGatewayWorker::makeWatchdogCallbacks(ModbusGatewayWorker *w)
{
    WatchdogTimer::Callbacks cb;
    // writeCoil: M112 flip at Heartbeat priority (level 3, §8.3).
    cb.writeCoil = [w](quint16 address, bool value, CommandPriority priority) {
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::WriteCoil;
        req.address = address;
        req.value = value ? 1 : 0;
        req.writeThenReadback = true;
        req.cls = requestClassFor(priority);
        if (!w->m_queue.enqueue(req))
            return false; // offline: skip the flip, never queue (spec §8.4)
        w->tryDispatch();
        return true;
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
    , m_watchdog(makeWatchdogCallbacks(this), [this]() { return m_nowMs(); })
    , m_nowMs([]() { return QDateTime::currentMSecsSinceEpoch(); })
{
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
    // nothing queued) and halt the M112 watchdog.
    m_pulses.reset();
    m_watchdog.setOnline(false);
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
    // Report any in-flight write, ack'd-but-unconfirmed write, and queued
    // write as failed — never silently dropped (IPlcGateway contract: results
    // arrive via writeCompleted()). stop() is BlockingQueued, so a stop->start
    // reconfiguration must not leave the UI waiting forever.
    reportDroppedWrites(QStringLiteral("gateway stopped"));
    m_queue.clear();
    m_inFlight = std::nullopt;
    // Reset the start guard and busy flag so a stop->start restart actually
    // re-creates/reopens the transport and runs again.
    m_started = false;
    m_busy = false;
    // Mirror the offline state so isOnline()/connectionStateChanged() do not
    // stay stale after stop (same signal enterOffline() emits).
    emit connectionStateChanged(false);
}

void ModbusGatewayWorker::submitWriteCoil(quint16 address, bool value, CommandPriority priority)
{
    ModbusRequest req;
    req.kind = ModbusRequest::Kind::WriteCoil;
    req.address = address;
    req.value = value ? 1 : 0;
    req.writeThenReadback = true; // spec §8.4: confirm writes by readback
    req.cls = (priority == CommandPriority::Safety) ? RequestClass::SafetyWrite
             : (priority == CommandPriority::PulseClear) ? RequestClass::PulseClear
             : (priority == CommandPriority::Heartbeat) ? RequestClass::Heartbeat
                                                        : RequestClass::UserWrite;
    if (!m_queue.enqueue(req)) {
        emit writeCompleted(address, false,
                            QStringLiteral("offline: command rejected, not replayed"));
        return;
    }
    tryDispatch();
}

void ModbusGatewayWorker::submitWriteRegister(quint16 address, quint16 value,
                                              CommandPriority priority)
{
    ModbusRequest req;
    req.kind = ModbusRequest::Kind::WriteRegister;
    req.address = address;
    req.value = value;
    req.writeThenReadback = true;
    req.cls = (priority == CommandPriority::Safety) ? RequestClass::SafetyWrite
             : (priority == CommandPriority::PulseClear) ? RequestClass::PulseClear
             : (priority == CommandPriority::Heartbeat) ? RequestClass::Heartbeat
                                                        : RequestClass::UserWrite;
    if (!m_queue.enqueue(req)) {
        emit writeCompleted(address, false,
                            QStringLiteral("offline: command rejected, not replayed"));
        return;
    }
    tryDispatch();
}

bool ModbusGatewayWorker::startPulse(quint16 address)
{
    // Offline (queue closed) or a pulse already active on this address:
    // rejected, nothing queued (spec §8.4, §8.5).
    return m_pulses.startPulse(address);
}

void ModbusGatewayWorker::onPollTick()
{
    // Drive the pulse hold timer and the M112 watchdog from the 50 ms poll
    // tick (spec §7.2, §8.5, §8.6). The watchdog only flips while online.
    m_pulses.onTick();
    if (m_watchdogEnabled)
        m_watchdog.onTick();

    if (m_state != LinkState::Online)
        return;

    const qint64 now = m_nowMs();
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
    m_state = LinkState::Connecting;
    m_watchdog.setOnline(false); // not online until the first full snapshot
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
    m_pendingConfirmations.clear();
    emit connectionStateChanged(false); // not fully online until first snapshot

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
    // nothing queued) and halt the M112 watchdog.
    m_pulses.reset();
    m_watchdog.setOnline(false);
    m_pulseReadbacks.clear();
    // Report any in-flight write, ack'd-but-unconfirmed write, and queued
    // write as failed — never silently dropped (IPlcGateway contract: results
    // arrive via writeCompleted()).
    reportDroppedWrites(QStringLiteral("offline"));
    m_queue.clear();
    emit connectionStateChanged(false);
    scheduleReconnect();
}

void ModbusGatewayWorker::reportDroppedWrites(const QString &reason)
{
    // An in-flight write must still report its result (IPlcGateway contract:
    // results arrive via writeCompleted()); otherwise the UI waits forever.
    if (m_inFlight
        && (m_inFlight->kind == ModbusRequest::Kind::WriteCoil
            || m_inFlight->kind == ModbusRequest::Kind::WriteRegister)) {
        emit writeCompleted(m_inFlight->address, false, reason);
    }
    m_inFlight = std::nullopt;
    // A write ack'd but not yet confirmed by readback, and a write queued but
    // not yet dispatched, must both report failure — never silently dropped.
    for (auto it = m_pendingConfirmations.constBegin();
         it != m_pendingConfirmations.constEnd(); ++it) {
        emit writeCompleted(it->address, false, reason);
    }
    m_pendingConfirmations.clear();
    ModbusRequest queued;
    while (m_queue.next(queued)) {
        if (queued.kind == ModbusRequest::Kind::WriteCoil
            || queued.kind == ModbusRequest::Kind::WriteRegister) {
            emit writeCompleted(queued.address, false, reason);
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
            // enterOffline() reports any in-flight write so it is not
            // silently dropped (IPlcGateway contract: results arrive via
            // writeCompleted()).
            enterOffline();
            return;
        }
        // Not yet offline (1st/2nd failure): report the write directly so it
        // is not silently dropped.
        if (req.kind == ModbusRequest::Kind::WriteCoil
            || req.kind == ModbusRequest::Kind::WriteRegister) {
            m_pulses.onWriteCompleted(req.address, false); // uncertain write (spec §8.5)
            emit writeCompleted(req.address, false,
                                QStringLiteral("send failed"));
        } else if (req.isReadback) {
            // A readback that could not be sent can never confirm the write:
            // fail it and drop the pending confirmation so it never leaks.
            const auto it = m_pendingConfirmations.constFind(req.requestId);
            if (it != m_pendingConfirmations.constEnd()) {
                emit writeCompleted(it->address, false,
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
        }
        if (req.kind == ModbusRequest::Kind::WriteCoil
            || req.kind == ModbusRequest::Kind::WriteRegister) {
            // A failed write must still report its result (spec §8.4,
            // IPlcGateway contract: results arrive via writeCompleted()).
            m_pulses.onWriteCompleted(req.address, false); // uncertain write (spec §8.5)
            emit writeCompleted(req.address, false, res.error);
        } else if (req.isReadback) {
            // A failed readback can never confirm the write: fail it and
            // drop the pending confirmation so it never leaks.
            const auto it = m_pendingConfirmations.constFind(req.requestId);
            if (it != m_pendingConfirmations.constEnd()) {
                emit writeCompleted(req.address, false,
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
                emit writeCompleted(req.address, true, QString());
            } else if (pw.retriesLeft > 0) {
                // Target not yet effective: retry per policy (spec §8.4).
                PendingWrite next = pw;
                --next.retriesLeft;
                m_pendingConfirmations.insert(req.requestId, next);
                ModbusRequest readback = req;
                readback.retriesLeft = 0;
                m_queue.enqueue(readback);
            } else {
                emit writeCompleted(req.address, false,
                                    QStringLiteral("write not confirmed by readback"));
            }
        }
        return;
    }

    if (req.cls == RequestClass::FastPoll) {
        if (res.values.size() >= kFastCount) {
            quint16 raw[41];
            for (int i = 0; i < kFastCount; ++i)
                raw[i] = res.values.at(i);
            const QDateTime started = QDateTime::currentDateTime();
            const qint64 ageMs = qMax<qint64>(0, m_nowMs() - m_fastDispatchedMs);
            // The fast block decode builds a fresh DeviceSnapshotData; the
            // slow-block fields (D204/D210/D220) and their out-of-range flags
            // are decoded by the slower SlowPoll and must survive the fast
            // poll, or the published snapshot would always show them as 0.
            const quint16 pulsePerMm = m_data.pulsePerMm;
            const qint16 widthDelta = m_data.widthDelta;
            const quint16 widthSpeed = m_data.widthSpeed;
            const quint32 slowInvalid = m_data.invalidFields
                & ((quint32(1) << quint8(SnapshotField::PulsePerMm))
                   | (quint32(1) << quint8(SnapshotField::WidthSpeed)));
            m_data = decodeFastBlock(raw, 0, true, ageMs, started, started,
                                     DataQuality::Valid);
            m_data.pulsePerMm = pulsePerMm;
            m_data.widthDelta = widthDelta;
            m_data.widthSpeed = widthSpeed;
            m_data.invalidFields |= slowInvalid;
            m_data.overallQuality = aggregateQuality(m_data);
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
                m_watchdog.setOnline(true); // M112 flips resume (spec §8.6)
                emit connectionStateChanged(true); // full snapshot acquired
            }
        }
        return;
    }

    if (req.cls == RequestClass::HomePoll) {
        if (!res.values.isEmpty())
            m_data.homeBits = res.values.first();
        return;
    }

    if (req.cls == RequestClass::CommandPoll) {
        if (!res.values.isEmpty())
            m_data.commandBits = res.values.first();
        return;
    }

    if (req.cls == RequestClass::SlowPoll) {
        if (res.values.size() >= 20) {
            m_data.pulsePerMm = res.values.at(0);   // D204
            m_data.widthDelta = decode::i16(res.values.at(6));  // D210
            m_data.widthSpeed = res.values.at(16);  // D220
            // Out-of-range D204/D220 mark the field invalid (spec §9) so the
            // UI shows "—" instead of a bogus value.
            checkSlowBlockRange(m_data);
        }
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
        emit writeCompleted(req.address, true, QString());
        return;
    }
    // Write acknowledged: confirm by readback (spec §8.4).
    PendingWrite pw;
    pw.address = req.address;
    pw.expected = req.value;
    pw.retriesLeft = m_cfg.readRetries;
    pw.requestId = req.id;
    m_pendingConfirmations.insert(req.id, pw);

    ModbusRequest readback;
    readback.kind = (req.kind == ModbusRequest::Kind::WriteCoil)
        ? ModbusRequest::Kind::ReadCoils
        : ModbusRequest::Kind::ReadRegisters;
    readback.address = req.address;
    readback.count = 1;
    readback.cls = RequestClass::UserWrite; // level 4: write-then-readback (§8.3)
    readback.isReadback = true;             // matched by request identity
    readback.requestId = req.id;
    m_queue.enqueue(readback);
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
    readback.cls = RequestClass::UserWrite; // level 4 (§8.3)
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

void ModbusGatewayWorker::publishSnapshot()
{
    m_data.connected = true;
    m_data.sequence = ++m_sequence;
    m_data.captureCompleted = QDateTime::currentDateTime();
    m_data.overallQuality = aggregateQuality(m_data);
    emit snapshotReady(DeviceSnapshot(m_data));
    // Communication statistics ride along with every published snapshot
    // (spec §16): the counters belong to the snapshot's sequence.
    emit commStatsChanged(m_sequence, m_reconnectCount, m_failedPolls);
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
