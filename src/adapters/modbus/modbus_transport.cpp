// Thin transport abstraction over QModbusRtuSerialClient (spec §7.2, §8.1).
// This TU exists so the Q_OBJECT meta-object for IModbusTransport is emitted
// into hlm_modbus; the real RtuTransport lives in qt_modbus_plc_gateway.cpp,
// fakes live in tests.
#include "adapters/modbus/modbus_transport.h"

namespace hlm {

QList<quint16> packCoilBits(const QList<quint16> &coilValues)
{
    QList<quint16> words;
    words.reserve((coilValues.size() + 15) / 16);
    for (int i = 0; i < coilValues.size(); ++i) {
        const int word = i / 16;
        if (word >= words.size())
            words.append(0);
        if (coilValues.at(i) != 0)
            words[word] = quint16(words.at(word) | (quint16(1) << (i % 16)));
    }
    // A coil read always yields at least one word, even for an empty payload:
    // callers index values[0] for the single-coil readback path.
    if (words.isEmpty())
        words.append(0);
    return words;
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
        // One 0/non-0 entry per coil -> packed words (one per 16 coils).
        res.values = packCoilBits(rawValues);
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
