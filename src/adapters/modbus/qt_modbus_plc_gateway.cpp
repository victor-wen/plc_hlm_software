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

// ---------------------------------------------------------------------------
// ModbusGatewayWorker
// ---------------------------------------------------------------------------
ModbusGatewayWorker::ModbusGatewayWorker(const QtModbusPlcGateway::Config &cfg,
                                         IModbusTransport *transport, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_transport(transport)
    , m_ownsTransport(transport == nullptr)
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
    m_queue.reopen();
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
    if (m_transport)
        m_transport->close();
    m_state = LinkState::Disconnected;
    m_queue.close();
    m_queue.clear();
    m_inFlight = std::nullopt;
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
                                                        : RequestClass::UserWrite;
    if (!m_queue.enqueue(req)) {
        emit writeCompleted(address, false,
                            QStringLiteral("offline: command rejected, not replayed"));
        return;
    }
    tryDispatch();
}

void ModbusGatewayWorker::onPollTick()
{
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
        m_queue.enqueue(req);
    }
    if (now - m_lastHomeMs >= m_homeMs) {
        m_lastHomeMs = now;
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::ReadCoils;
        req.address = kHomeStart;
        req.count = kHomeCount;
        req.cls = RequestClass::HomePoll;
        req.retriesLeft = m_cfg.readRetries;
        m_queue.enqueue(req);
    }
    if (now - m_lastCommandMs >= m_commandMs) {
        m_lastCommandMs = now;
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::ReadCoils;
        req.address = kCommandStart;
        req.count = kCommandCount;
        req.cls = RequestClass::CommandPoll;
        req.retriesLeft = m_cfg.readRetries;
        m_queue.enqueue(req);
    }
    if (now - m_lastSlowMs >= m_slowMs) {
        m_lastSlowMs = now;
        ModbusRequest req;
        req.kind = ModbusRequest::Kind::ReadRegisters;
        req.address = kSlowStart;
        req.count = kSlowCount;
        req.cls = RequestClass::SlowPoll;
        req.retriesLeft = m_cfg.readRetries;
        m_queue.enqueue(req);
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
    if (!m_transport->open()) {
        enterOffline();
        return;
    }
    m_state = LinkState::Online;
    m_queue.reopen();
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
    // promptly after (re)connect (spec §8.4).
    ModbusRequest fast;
    fast.kind = ModbusRequest::Kind::ReadRegisters;
    fast.address = kFastStart;
    fast.count = kFastCount;
    fast.cls = RequestClass::FastPoll;
    fast.retriesLeft = m_cfg.readRetries;
    m_queue.enqueue(fast);

    tryDispatch();
}

void ModbusGatewayWorker::enterOffline()
{
    m_state = LinkState::Offline;
    m_queue.close();
    m_queue.clear();
    m_hasValidSnapshot = false;
    m_busy = false;
    m_inFlight = std::nullopt;
    m_pendingConfirmations.clear();
    emit connectionStateChanged(false);
    scheduleReconnect();
}

void ModbusGatewayWorker::scheduleReconnect()
{
    m_reconnectTimer->start(m_policy.nextReconnectDelayMs());
}

void ModbusGatewayWorker::tryDispatch()
{
    if (m_busy || m_state != LinkState::Online)
        return;

    ModbusRequest req;
    if (!m_queue.next(req))
        return;

    if (!m_transport->send(req)) {
        // Link dropped mid-flight; treat as a transfer failure.
        m_policy.onTransferFailure();
        if (m_policy.isOffline())
            enterOffline();
        return;
    }
    if (req.cls == RequestClass::FastPoll)
        m_fastDispatchedMs = m_nowMs(); // for dataAgeMs of the next snapshot
    m_inFlight = req;
    m_busy = true;
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
        }
        if (req.kind == ModbusRequest::Kind::WriteCoil
            || req.kind == ModbusRequest::Kind::WriteRegister) {
            // A failed write must still report its result (spec §8.4,
            // IPlcGateway contract: results arrive via writeCompleted()).
            emit writeCompleted(req.address, false, res.error);
        } else if (req.isReadback) {
            // A failed readback can never confirm the write: fail it and
            // drop the pending confirmation so it never leaks.
            const auto it = m_pendingConfirmations.constFind(req.requestId);
            if (it != m_pendingConfirmations.constEnd()) {
                emit writeCompleted(req.address, false,
                                    QStringLiteral("readback transfer failed"));
                m_pendingConfirmations.erase(it);
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
            m_data = decodeFastBlock(raw, 0, true, ageMs, started, started,
                                     DataQuality::Valid);
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
        }
        return;
    }
}

void ModbusGatewayWorker::handleWriteResult(const ModbusRequest &req, const TransferResult &res)
{
    Q_UNUSED(res);
    if (!req.writeThenReadback) {
        emit writeCompleted(req.address, true, QString());
        return;
    }
    // Write acknowledged: confirm by readback (spec §8.4).
    PendingWrite pw;
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

void ModbusGatewayWorker::publishSnapshot()
{
    m_data.connected = true;
    m_data.sequence = ++m_sequence;
    m_data.captureCompleted = QDateTime::currentDateTime();
    m_data.overallQuality = aggregateQuality(m_data);
    emit snapshotReady(DeviceSnapshot(m_data));
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
