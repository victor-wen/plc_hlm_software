#pragma once

#include <QObject>
#include <QSerialPort>
#include <QThread>
#include <QTimer>

#include <functional>
#include <optional>

#include "adapters/modbus/modbus_transport.h"
#include "adapters/modbus/reconnect_policy.h"
#include "adapters/modbus/request_queue.h"
#include "ports/iplc_gateway.h"

namespace hlm {

// Real PLC gateway (spec §7.2, §8). Facade living in the caller thread; all
// Modbus work happens on a dedicated worker thread that owns the
// QModbusRtuSerialClient (created there, never moved across threads).
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
    };

    explicit QtModbusPlcGateway(const Config &cfg, QObject *parent = nullptr);
    ~QtModbusPlcGateway() override;

    void start() override;
    void stop() override;
    bool isOnline() const override;

    void writeCoil(quint16 address, bool value,
                   CommandPriority priority = CommandPriority::Normal) override;
    void writeRegister(quint16 address, quint16 value,
                       CommandPriority priority = CommandPriority::Normal) override;

private:
    Config m_cfg;
    QThread m_thread;
    ModbusGatewayWorker *m_worker = nullptr;
    bool m_online = false; // mirrored from connectionStateChanged (UI thread)
    bool m_started = false; // guards against double start()
};

// Worker: owns the transport, request queue, reconnect policy, polling timers
// and the transfer state machine. Runs entirely on the gateway's worker
// thread. Public slots double as deterministic test hooks (drive directly
// with a fake transport and an injected clock).
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

public slots:
    void start();
    void stop();
    void submitWriteCoil(quint16 address, bool value, CommandPriority priority);
    void submitWriteRegister(quint16 address, quint16 value, CommandPriority priority);
    void onPollTick();
    void onReconnectTick();
    void onTransferFinished(const TransferResult &result);

signals:
    void snapshotReady(const DeviceSnapshot &snapshot);
    void connectionStateChanged(bool online);
    void writeCompleted(quint16 address, bool ok, const QString &error);

private:
    enum class LinkState { Disconnected, Connecting, Online, Offline };

    void openLink();
    void enterOffline();
    void reportDroppedWrites(const QString &reason);
    void scheduleReconnect();
    void tryDispatch();
    void handleReadResult(const ModbusRequest &req, const TransferResult &res);
    void handleWriteResult(const ModbusRequest &req, const TransferResult &res);
    void publishSnapshot();
    void checkHeartbeatFreeze(quint16 heartbeat);

    struct PendingWrite {
        quint16 address = 0; // address of the original write request
        quint16 expected = 0;
        int retriesLeft = 0;
        quint64 requestId = 0; // id of the original write request
    };

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

    // D140 heartbeat freeze detection (spec §8.4).
    quint16 m_lastHeartbeat = 0;
    bool m_haveHeartbeat = false;
    qint64 m_lastHeartbeatChangeMs = 0;

    // Accumulated snapshot data (fast block refreshed every 250 ms).
    DeviceSnapshotData m_data;
    // Pending write confirmations keyed by the id of the original write
    // request, so duplicate writes to the same address each get their own
    // confirmation (spec §8.4).
    QHash<quint64, PendingWrite> m_pendingConfirmations;

    std::function<qint64()> m_nowMs;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;
};

} // namespace hlm
