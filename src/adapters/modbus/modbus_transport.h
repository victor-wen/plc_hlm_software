#pragma once

#include <QList>
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

// Packs a ReadCoils result into the single word required by
// TransferResult::values. Input is one entry per coil (0/1, LSB-first as
// returned by QModbusDataUnit); output has bit i = coil i. A single-coil
// readback (count == 1) is therefore unchanged: bit0 == the coil value.
// Blocks are <= 16 coils (command 13, home 4), so one quint16 suffices.
quint16 packCoilBits(const QList<quint16> &coilValues);

// Builds the TransferResult for one completed request from the raw values the
// transport read. This is the SINGLE place the Modbus reply-to-result
// conversion lives (qt_modbus_plc_gateway.cpp's RtuTransport only calls it),
// so a unit test here locks the gateway wiring:
//   - ReadCoils: rawValues is one 0/non-0 entry per coil -> packed into a
//     single values[0] by packCoilBits (bit i = coil i).
//   - ReadRegisters: register values are passed through unchanged, one entry
//     each.
//   - WriteCoil/WriteRegister: no read payload (values stays empty).
// On failure (ok == false) the error is propagated and values is empty.
TransferResult makeTransferResult(const ModbusRequest &req, bool ok,
                                  const QString &error,
                                  const QList<quint16> &rawValues);

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
