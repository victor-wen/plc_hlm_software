// Thin transport abstraction over QModbusRtuSerialClient (spec §7.2, §8.1).
// This TU exists so the Q_OBJECT meta-object for IModbusTransport is emitted
// into hlm_modbus; the real RtuTransport lives in qt_modbus_plc_gateway.cpp,
// fakes live in tests.
#include "adapters/modbus/modbus_transport.h"
