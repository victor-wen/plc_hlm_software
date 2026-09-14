// Thin transport abstraction over QModbusRtuSerialClient (spec §7.2, §8.1).
// This TU exists so the Q_OBJECT meta-object for IModbusTransport is emitted
// into hlm_modbus; the real RtuTransport lives in qt_modbus_plc_gateway.cpp,
// fakes live in tests.
#include "adapters/modbus/modbus_transport.h"

namespace hlm {

quint16 packCoilBits(const QList<quint16> &coilValues)
{
    quint16 packed = 0;
    const int count = qMin(coilValues.size(), 16);
    for (int i = 0; i < count; ++i) {
        if (coilValues.at(i) != 0)
            packed |= quint16(1) << i;
    }
    return packed;
}

TransferResult makeTransferResult(const ModbusRequest &req, bool ok,
                                  const QString &error,
                                  const QList<quint16> &rawValues)
{
    TransferResult res;
    res.ok = ok;
    res.error = error;
    if (!ok)
        return res;

    switch (req.kind) {
    case ModbusRequest::Kind::ReadCoils:
        // One 0/non-0 entry per coil -> single packed word.
        res.values.append(packCoilBits(rawValues));
        break;
    case ModbusRequest::Kind::ReadRegisters:
        for (quint16 value : rawValues)
            res.values.append(value);
        break;
    case ModbusRequest::Kind::WriteCoil:
    case ModbusRequest::Kind::WriteRegister:
        break; // writes carry no read payload
    }
    return res;
}

} // namespace hlm
