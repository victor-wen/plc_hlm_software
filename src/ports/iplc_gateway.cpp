// Port interface (spec §7.2, §8). Header-only interface; this TU exists so
// the Q_OBJECT meta-object for IPlcGateway is emitted into hlm_core, letting
// both the real Modbus gateway and the in-process SimulatedPlcGateway
// implement it, plus the queued-cross-thread value type registrations.
#include "ports/iplc_gateway.h"

namespace hlm {

void registerPlcGatewayMetaTypes()
{
    qRegisterMetaType<DeviceSnapshot>();
    qRegisterMetaType<CommandPriority>();
    qRegisterMetaType<PlcOperation>();
    qRegisterMetaType<SubmissionResult>();
    qRegisterMetaType<SubmissionCompletion>();
    qRegisterMetaType<PlcCommStats>();
}

} // namespace hlm
