// Port interface (spec §6, §7.4, §13). Header-only interface; this TU exists
// so the Q_OBJECT meta-object for IVisionService is emitted into hlm_core,
// letting the OpenCV adapter (Task 9) and any future camera adapter implement
// it without leaking OpenCV types into the domain layer.
#include "ports/ivision_service.h"
