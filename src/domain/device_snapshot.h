#pragma once

#include <QDateTime>
#include <QMetaType>

#include "domain/fault_code.h"
#include "domain/quality.h"

namespace hlm {

// Fields of the fast status block (D100-D140) that carry a defined range.
// Used to mark individual fields invalid when their decoded value is
// out of range (spec §9).
enum class SnapshotField : quint8 {
    FaultCode = 0,   // D110, 0-10
    CurrentStep,     // D120, 0-5
    BeltSpeed,       // D122, 100-20000
    TargetWidth,     // D128, no range (user decision 2026-09-21: any u16)
    CurrentWidth,    // D130, no range (user decision 2026-09-21: any u16)
    PulsePerMm,      // D204, 1-32767
    WidthSpeed,      // D220, 1-15
    Heartbeat,       // D140, no range (always valid)
    FieldCount
};

// Raw decoded values plus per-block quality. This is the mutable carrier
// used to build an immutable DeviceSnapshot; it is not published directly.
struct DeviceSnapshotData {
    QDateTime captureStarted;
    QDateTime captureCompleted;
    quint64 sequence = 0;
    bool connected = false;
    qint64 dataAgeMs = 0;

    // D100-D105 raw status words.
    quint16 statusWord1 = 0; // D100
    quint16 statusWord2 = 0; // D102
    quint16 statusWord3 = 0; // D103
    quint16 statusWord4 = 0; // D104
    quint16 statusWord5 = 0; // D105

    // Decoded fast-block fields.
    quint16 faultCode = 0;      // D110
    quint16 currentStep = 0;    // D120
    quint16 beltSpeed = 0;      // D122
    quint32 widthFrequency = 0; // D126 low + D127 high
    // Width registers are carried in 0.1 mm units (user decision 2026-09-21):
    // raw 2000 is 200.0 mm. See domain/width_units.h for the conversion.
    quint16 targetWidth = 0;    // D128 (0.1 mm)
    quint16 currentWidth = 0;   // D130 (0.1 mm)
    qint32 pulseCount = 0;      // D136 low + D137 high (int32)
    quint32 productionCount = 0;// D138 low + D139 high (uint32)
    quint16 heartbeat = 0;      // D140

    // Slow-block fields.
    quint16 pulsePerMm = 0; // D204
    qint16 widthDelta = 0;  // D210 (int16, target - current, 0.1 mm)
    quint16 widthSpeed = 0; // D220

    // M50-M53 home-return bits (function code 01).
    quint16 homeBits = 0;
    // M15 扫码结束 (user decision 2026-09-22): read as a coil (function code
    // 01) in the same block as the home bits, never from D100 bit15. The PLC
    // program does not drive M15 yet — the scan program / PLC engineer adds it.
    bool scanComplete = false;
    // M100-M111 command readback bits (function code 01). M112 is not part of
    // the live path (PLC-HMI-003 D3).
    quint16 commandBits = 0;

    // Per-source-block quality and monotonic ages (spec §9, PLC-HMI-003 D6).
    // Member names are fixed by the approved contract interfaces.
    DataQuality fast_quality = DataQuality::Valid;
    qint64 fast_age_ms = 0;
    DataQuality home_quality = DataQuality::Valid;
    qint64 home_age_ms = 0;
    DataQuality command_quality = DataQuality::Valid;
    qint64 command_age_ms = 0;
    DataQuality slow_quality = DataQuality::Valid;
    qint64 slow_age_ms = 0;

    // Bitmask of fields whose decoded value is out of range.
    quint32 invalidFields = 0;
    DataQuality overall_quality = DataQuality::Valid;
    qint64 overall_age_ms = 0;
    // Independent D210 metadata (never aliases CurrentWidth validity):
    // true only with a valid slow block. The former signed -350..350 window
    // was derived from the retired 50-400 mm operator envelope, so it was
    // removed with it (user decision 2026-09-21).
    bool width_delta_valid = false;
};

// Aggregates per-block quality and out-of-range flags into the overall
// snapshot quality (worst wins; OutOfRange beats ProtocolError).
DataQuality aggregateQuality(const DeviceSnapshotData &d);

// Recomputes overall_age_ms (maximum of the required source blocks; never
// hard-coded zero) and width_delta_valid (valid slow block) from the block
// fields.
void recomputeDerivedQuality(DeviceSnapshotData &d);

// Centralized decode helpers (spec §8.2). All 32-bit values are low word
// first (D126/D127, D136/D137, D138/D139).
namespace decode {
qint16 i16(quint16 raw);
qint32 i32(quint16 low, quint16 high);
quint32 u32(quint16 low, quint16 high);
bool d100Bit(quint16 statusWord1, int bit); // bit0-14 -> M0-M14
bool d103Bit(quint16 statusWord3, int bit); // bit0-15 -> M30-M45
bool heartbeatActive(quint16 previous, quint16 current);
} // namespace decode

// Decodes the fast status block (D100-D140, 41 registers) into a
// DeviceSnapshotData. `sequence` is the snapshot sequence number,
// `connected` the link state, `dataAgeMs` the age of the block, and
// `quality` the block quality. Out-of-range values mark the affected field
// invalid (spec §9).
DeviceSnapshotData decodeFastBlock(const quint16 raw[41], quint64 sequence,
                                   bool connected, qint64 dataAgeMs,
                                   const QDateTime &captureStarted,
                                   const QDateTime &captureCompleted,
                                   DataQuality quality);

// Marks slow-block fields (D204 PulsePerMm, D220 WidthSpeed) invalid when
// their decoded value is out of range (spec §9). Called by the slow-block
// decode paths (gateway SlowPoll, simulator) before the snapshot is
// published, so the UI shows "—" instead of an out-of-range value.
void checkSlowBlockRange(DeviceSnapshotData &d);

// Immutable device snapshot (spec §9). Constructed atomically from a
// DeviceSnapshotData; exposes only const accessors, no setters.
class DeviceSnapshot
{
public:
    explicit DeviceSnapshot(const DeviceSnapshotData &d);

    // --- quality and age (PLC-HMI-003 D6) -----------------------------------
    // Contract-fixed member names: real per-block quality and monotonic ages,
    // the overall maximum age and the independent D210 validity flag.
    DataQuality fast_quality = DataQuality::Valid;
    qint64 fast_age_ms = 0;
    DataQuality home_quality = DataQuality::Valid;
    qint64 home_age_ms = 0;
    DataQuality command_quality = DataQuality::Valid;
    qint64 command_age_ms = 0;
    DataQuality slow_quality = DataQuality::Valid;
    qint64 slow_age_ms = 0;
    DataQuality overall_quality = DataQuality::Valid;
    qint64 overall_age_ms = 0;
    bool width_delta_valid = false;

    // --- meta ---------------------------------------------------------------
    QDateTime captureStarted() const { return m_captureStarted; }
    QDateTime captureCompleted() const { return m_captureCompleted; }
    quint64 sequence() const { return m_sequence; }
    bool connected() const { return m_connected; }
    qint64 dataAgeMs() const { return m_dataAgeMs; }
    DataQuality overallQuality() const { return m_overall_quality; }

    // --- raw status words ---------------------------------------------------
    quint16 statusWord1() const { return m_statusWord1; }
    quint16 statusWord2() const { return m_statusWord2; }
    quint16 statusWord3() const { return m_statusWord3; }
    quint16 statusWord4() const { return m_statusWord4; }
    quint16 statusWord5() const { return m_statusWord5; }

    // --- D100 bit states (M0-M14) -------------------------------------------
    bool m0() const { return m_m0; }
    bool m1() const { return m_m1; }
    bool m2() const { return m_m2; }
    bool m3() const { return m_m3; }
    bool m4() const { return m_m4; }
    bool m5() const { return m_m5; }
    bool m6() const { return m_m6; }
    bool m7() const { return m_m7; }
    bool m8() const { return m_m8; }
    bool m9() const { return m_m9; }
    bool m10() const { return m_m10; }
    bool m11() const { return m_m11; }
    bool m12() const { return m_m12; }
    bool m13() const { return m_m13; }
    bool m14() const { return m_m14; }

    // --- D103 bit states (M30-M45) ------------------------------------------
    bool m30() const { return m_m30; }
    bool m31() const { return m_m31; }
    bool m32() const { return m_m32; }
    bool m33() const { return m_m33; }
    bool m34() const { return m_m34; }
    bool m35() const { return m_m35; }
    bool m40() const { return m_m40; }
    bool m41() const { return m_m41; }
    bool m42() const { return m_m42; }
    bool m43() const { return m_m43; }
    bool m44() const { return m_m44; }
    bool m45() const { return m_m45; }

    // --- decoded fields ------------------------------------------------------
    quint16 faultCode() const { return m_faultCode; }
    const FaultInfo &fault() const { return m_fault; }
    quint16 currentStep() const { return m_currentStep; }
    quint16 beltSpeed() const { return m_beltSpeed; }
    quint32 widthFrequency() const { return m_widthFrequency; }
    quint16 targetWidth() const { return m_targetWidth; }
    quint16 currentWidth() const { return m_currentWidth; }
    qint32 pulseCount() const { return m_pulseCount; }
    quint32 productionCount() const { return m_productionCount; }
    quint16 heartbeat() const { return m_heartbeat; }
    quint16 pulsePerMm() const { return m_pulsePerMm; }
    qint16 widthDelta() const { return m_widthDelta; }
    quint16 widthSpeed() const { return m_widthSpeed; }

    // --- M50-M53 home-return bits -------------------------------------------
    bool m50() const { return m_m50; }
    bool m51() const { return m_m51; }
    bool m52() const { return m_m52; }
    bool m53() const { return m_m53; }

    // --- M15 扫码结束 (coil, user decision 2026-09-22) ------------------------
    // The scan program is external: the PLC raises M15 when a scan cycle ends,
    // and the HMI reads the barcode result file on its rising edge.
    bool m15() const { return m_scanComplete; }

    // --- M100-M111 command readback (M112 removed, PLC-HMI-003 D3) ----------
    bool m100() const { return m_m100; }
    bool m101() const { return m_m101; }
    bool m102() const { return m_m102; }
    bool m103() const { return m_m103; }
    bool m104() const { return m_m104; }
    bool m105() const { return m_m105; }
    bool m106() const { return m_m106; }
    bool m107() const { return m_m107; }
    bool m108() const { return m_m108; }
    bool m109() const { return m_m109; }
    bool m110() const { return m_m110; }
    bool m111() const { return m_m111; }

    // --- per-source-block quality --------------------------------------------
    DataQuality fastQuality() const { return fast_quality; }
    DataQuality homeQuality() const { return home_quality; }
    DataQuality commandQuality() const { return command_quality; }
    DataQuality slowQuality() const { return slow_quality; }

    // True when the given field decoded in range (spec §9).
    bool fieldValid(SnapshotField f) const
    {
        return (m_invalidFields & (quint32(1) << quint8(f))) == 0;
    }

private:
    QDateTime m_captureStarted;
    QDateTime m_captureCompleted;
    quint64 m_sequence = 0;
    bool m_connected = false;
    qint64 m_dataAgeMs = 0;
    DataQuality m_overall_quality = DataQuality::Valid;

    quint16 m_statusWord1 = 0;
    quint16 m_statusWord2 = 0;
    quint16 m_statusWord3 = 0;
    quint16 m_statusWord4 = 0;
    quint16 m_statusWord5 = 0;

    bool m_m0 = false;
    bool m_m1 = false;
    bool m_m2 = false;
    bool m_m3 = false;
    bool m_m4 = false;
    bool m_m5 = false;
    bool m_m6 = false;
    bool m_m7 = false;
    bool m_m8 = false;
    bool m_m9 = false;
    bool m_m10 = false;
    bool m_m11 = false;
    bool m_m12 = false;
    bool m_m13 = false;
    bool m_m14 = false;

    bool m_m30 = false;
    bool m_m31 = false;
    bool m_m32 = false;
    bool m_m33 = false;
    bool m_m34 = false;
    bool m_m35 = false;
    bool m_m40 = false;
    bool m_m41 = false;
    bool m_m42 = false;
    bool m_m43 = false;
    bool m_m44 = false;
    bool m_m45 = false;

    quint16 m_faultCode = 0;
    FaultInfo m_fault;
    quint16 m_currentStep = 0;
    quint16 m_beltSpeed = 0;
    quint32 m_widthFrequency = 0;
    quint16 m_targetWidth = 0;
    quint16 m_currentWidth = 0;
    qint32 m_pulseCount = 0;
    quint32 m_productionCount = 0;
    quint16 m_heartbeat = 0;
    quint16 m_pulsePerMm = 0;
    qint16 m_widthDelta = 0;
    quint16 m_widthSpeed = 0;

    bool m_m50 = false;
    bool m_m51 = false;
    bool m_m52 = false;
    bool m_m53 = false;
    bool m_scanComplete = false; // M15 (coil, not a D100 bit)

    bool m_m100 = false;
    bool m_m101 = false;
    bool m_m102 = false;
    bool m_m103 = false;
    bool m_m104 = false;
    bool m_m105 = false;
    bool m_m106 = false;
    bool m_m107 = false;
    bool m_m108 = false;
    bool m_m109 = false;
    bool m_m110 = false;
    bool m_m111 = false;

    quint32 m_invalidFields = 0;
};

} // namespace hlm

Q_DECLARE_METATYPE(hlm::DeviceSnapshot)
