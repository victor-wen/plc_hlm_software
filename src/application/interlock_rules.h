#pragma once

#include <QStringList>

#include "domain/device_snapshot.h"

namespace hlm {

// Structured interlock result: allowed + ordered unmet preconditions for UI
// display (spec §10, §11.2 "命令不得乐观更新状态").
struct InterlockResult {
    bool allowed = false;
    QStringList unmet; // ordered, human-readable; empty when allowed
};

// Pure per-command precondition rules (spec §10). No state, no I/O.
// M60/M61 are read via their D100-mapped bits M8/M9 (spec §8.2).
class InterlockRules
{
public:
    static InterlockResult checkReset(const DeviceSnapshot &s, bool online);
    static InterlockResult checkAdjustWidth(const DeviceSnapshot &s, bool online,
                                            quint16 targetWidth);
    static InterlockResult checkStart(const DeviceSnapshot &s, bool online);
    static InterlockResult checkStop(const DeviceSnapshot &s, bool online);
    static InterlockResult checkEstopSet(const DeviceSnapshot &s, bool online);
    static InterlockResult checkEstopRelease(const DeviceSnapshot &s, bool online);
    static InterlockResult checkModeSwitch(const DeviceSnapshot &s, bool online);
    static InterlockResult checkManualCommand(const DeviceSnapshot &s, bool online);
    static InterlockResult checkBypass(const DeviceSnapshot &s, bool online);
};

} // namespace hlm
