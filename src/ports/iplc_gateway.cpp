// Port interface (spec §7.2, §8). Header-only interface; this TU exists so
// the Q_OBJECT meta-object for IPlcGateway is emitted into hlm_core, letting
// both the real Modbus gateway (Task 4) and the in-process SimulatedPlcGateway
// (Task 6) implement it.
#include "ports/iplc_gateway.h"
