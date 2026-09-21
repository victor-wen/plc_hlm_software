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
//
// PLC-HMI-011 D5: the manual rule is address-aware. M106/M107 (手动调宽) also
// require M61=1 (回原点完成) and M50=0 (未在回原点); M108 (皮带点动) and
// M109 (挡停) keep the common gates only, because the ladder's SBR_MANUAL
// comments state M108 手动皮带点动 不需要复位完成. The default address 0
// preserves the historical four-command verdict for callers that have no
// address context (e.g. the pre-split page gate).
class InterlockRules
{
public:
    static InterlockResult checkReset(const DeviceSnapshot &s, bool online);
    // 回原点 (M50=1 持续写). Gates: online + manual mode + M3=0 + M0=0 +
    // M14=0 + M50=0. User decision (2026-09-21): homing is its own command.
    static InterlockResult checkHomeStart(const DeviceSnapshot &s, bool online);
    static InterlockResult checkAdjustWidth(const DeviceSnapshot &s, bool online,
                                            quint16 targetWidth);
    static InterlockResult checkStart(const DeviceSnapshot &s, bool online);
    static InterlockResult checkStop(const DeviceSnapshot &s, bool online);
    static InterlockResult checkEstopSet(const DeviceSnapshot &s, bool online);
    static InterlockResult checkEstopRelease(const DeviceSnapshot &s, bool online);
    static InterlockResult checkModeSwitch(const DeviceSnapshot &s, bool online);
    // `address` is the manual coil (M106-M109) and only selects the command
    // identity today. User decision (2026-09-21): no manual command requires
    // homing completion (M61) or an idle homing bit (M50). The common gates
    // apply to all four: online + manual mode + M3=0 + M0=0 + M14=0.
    static InterlockResult checkManualCommand(const DeviceSnapshot &s, bool online,
                                              quint16 address = 0);
    static InterlockResult checkBypass(const DeviceSnapshot &s, bool online);
};

} // namespace hlm
