#pragma once

#include <QString>

#include "domain/device_snapshot.h"

namespace hlm {

class ShellModel;

// One rendered field: display text plus validity. `valid` false renders as
// "—" (spec §9: 字段过期或无效时显示"—").
struct OverviewField
{
    QString text;
    bool valid = false;
};

// Maps the latest ShellModel snapshot to the 总览 page display fields
// (spec §9, §11.3). Pure read-only mapping over ShellModel: no I/O, no
// commands, no state of its own. Every call re-reads the CURRENT snapshot
// in full — there are no per-field or optimistic updates.
//
// A field is valid only when the whole snapshot is fresh (ShellModel::
// snapshotFresh: connected + all block qualities Valid) AND the individual
// field decoded in range (DeviceSnapshot::fieldValid). This matches the
// shell semantics: a stale snapshot blanks the whole panel rather than
// mixing trusted and untrusted values.
class OverviewModel
{
public:
    explicit OverviewModel(const ShellModel &model);

    // --- per-field mapping (register in comment) -----------------------------
    OverviewField step() const;             // D120 当前步骤
    OverviewField targetWidth() const;      // D128 目标宽度 (mm)
    OverviewField currentWidth() const;     // D130 当前宽度 (mm)
    OverviewField widthDelta() const;       // D210 调宽差值 (mm, signed)
    OverviewField beltSpeed() const;        // D122 皮带速度 (Hz)
    OverviewField productionCount() const;  // D138 累计产量 (件)

    // --- status flags (snapshot-confirmed only) -------------------------------
    bool online() const;
    bool modeKnown() const;
    bool isAutoMode() const;
    bool isRunning() const;
    bool isFaulted() const;

    // Latest alarm line for the page (same priority as the shell banner):
    // estop > latched fault > fault > offline notice; 无报警 when healthy.
    QString latestAlarmText() const;

private:
    bool fresh() const;
    OverviewField field(const QString &text, quint8 f) const;

    const ShellModel &m_model;
};

} // namespace hlm