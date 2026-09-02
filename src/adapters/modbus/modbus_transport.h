#pragma once

#include <QObject>
#include <QString>

#include "adapters/modbus/request_queue.h"

namespace hlm {

// Result of one Modbus transfer. Value object; delivered via the transport's
// signals. `ok` is false on any transport error (timeout, protocol, CRC).
struct TransferResult {
    bool ok = false;
    QString error;
    // Read payload. For ReadRegisters: raw register values (count entries).
    // For ReadCoils: one bit per coil packed into values[0] (bit i = coil i).
    QList<quint16> values;
};

// Thin transport abstraction over QModbusRtuSerialClient (spec §7.2, §8.1).
// The gateway owns exactly one transport, created and used only on the
// gateway's own worker thread. A fake transport is used in unit tests.
class IModbusTransport : public QObject
{
    Q_OBJECT

public:
    explicit IModbusTransport(QObject *parent = nullptr) : QObject(parent) {}
    ~IModbusTransport() override = default;

    // Open the serial link. Returns false on configuration/connect failure.
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Send one request. The transport emits transferFinished() exactly once
    // per accepted request (success or failure). Returns false when the
    // request could not be sent (link down).
    virtual bool send(const ModbusRequest &req) = 0;

signals:
    void transferFinished(const hlm::TransferResult &result);
};

} // namespace hlm
