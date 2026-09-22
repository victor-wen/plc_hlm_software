#include "domain/device_snapshot.h"

#include <algorithm>

namespace hlm {

namespace {

// Marks a field invalid when its decoded value is outside the defined range
// (spec §9). Ranges come from 需求/PLC上位机地址及要求.txt.
void checkRange(DeviceSnapshotData &d, SnapshotField f, quint16 value,
                quint16 min, quint16 max)
{
    if (value < min || value > max)
        d.invalidFields |= (quint32(1) << quint8(f));
}

// M0-M14 source: the M0-M15 coil read OR the D100 mirror bit (user decision
// 2026-09-22, see DeviceSnapshotData::statusCoils1). The OR is deliberate: the
// supplied PLC program builds 状态字1 with MOV K2M0 D100, which copies only
// M0-M7, so D100's high byte reads 0 forever even when M8/M9/M14 are set — the
// coil read is the live truth. Keeping the mirror as a second source can only
// add a warning, never hide one, and on a corrected PLC (MOV K4M0) both agree.
bool statusBitFromEitherSource(const DeviceSnapshotData &d, int bit)
{
    return decode::d100Bit(d.statusWord1, bit)
        || (d.statusCoils1 & (quint16(1) << bit)) != 0;
}

// M30-M45 source: the coil read OR the D103 mirror bit (user decision
// 2026-09-22). Same reasoning as above: 状态字3 is built with MOV K2M30 D103,
// so its high byte (M38-M45, including 调宽成功/失败 and 皮带常转) is dead on
// the supplied program.
bool statusBit3FromEitherSource(const DeviceSnapshotData &d, int bit)
{
    return decode::d103Bit(d.statusWord3, bit)
        || (d.statusCoils3 & (quint16(1) << bit)) != 0;
}

} // namespace

bool DeviceSnapshot::statusBit(int mNumber) const
{
    switch (mNumber) {
    case 0: return m_m0;
    case 1: return m_m1;
    case 2: return m_m2;
    case 3: return m_m3;
    case 4: return m_m4;
    case 5: return m_m5;
    case 6: return m_m6;
    case 7: return m_m7;
    case 8: return m_m8;
    case 9: return m_m9;
    case 10: return m_m10;
    case 11: return m_m11;
    case 12: return m_m12;
    case 13: return m_m13;
    case 14: return m_m14;
    case 30: return m_m30;
    case 31: return m_m31;
    case 32: return m_m32;
    case 33: return m_m33;
    case 34: return m_m34;
    case 35: return m_m35;
    case 40: return m_m40;
    case 41: return m_m41;
    case 42: return m_m42;
    case 43: return m_m43;
    case 44: return m_m44;
    case 45: return m_m45;
    default: return false; // bit15 of D100 and undefined M numbers
    }
}

DataQuality aggregateQuality(const DeviceSnapshotData &d)
{
    DataQuality q = d.fast_quality;
    q = worstQuality(q, d.home_quality);
    q = worstQuality(q, d.command_quality);
    q = worstQuality(q, d.slow_quality);
    if (d.invalidFields != 0)
        q = worstQuality(q, DataQuality::OutOfRange);
    return q;
}

void recomputeDerivedQuality(DeviceSnapshotData &d)
{
    // Overall age is the maximum age of the source blocks required by the
    // published snapshot; it is never a hard-coded zero (contract D6).
    d.overall_age_ms = std::max(
        {d.fast_age_ms, d.home_age_ms, d.command_age_ms, d.slow_age_ms});
    // WidthDelta validity requires a valid slow block. The former signed
    // -350..350 window came from the retired 50-400 mm operator envelope and
    // was removed with it (user decision 2026-09-21); it still never aliases
    // CurrentWidth validity.
    d.width_delta_valid = d.slow_quality == DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
}

namespace decode {

qint16 i16(quint16 raw)
{
    return static_cast<qint16>(raw);
}

qint32 i32(quint16 low, quint16 high)
{
    const quint32 u = (quint32(high) << 16) | quint32(low);
    return static_cast<qint32>(u);
}

quint32 u32(quint16 low, quint16 high)
{
    return (quint32(high) << 16) | quint32(low);
}

bool d100Bit(quint16 statusWord1, int bit)
{
    if (bit < 0 || bit > 14)
        return false; // bit15 is reserved (spec §8.2)
    return (statusWord1 & (quint16(1) << bit)) != 0;
}

bool d103Bit(quint16 statusWord3, int bit)
{
    if (bit < 0 || bit > 15)
        return false;
    return (statusWord3 & (quint16(1) << bit)) != 0;
}

bool heartbeatActive(quint16 previous, quint16 current)
{
    return previous != current; // value change only; 16-bit wrap is fine
}

} // namespace decode

DeviceSnapshotData decodeFastBlock(const quint16 raw[41], quint64 sequence,
                                   bool connected, qint64 dataAgeMs,
                                   const QDateTime &captureStarted,
                                   const QDateTime &captureCompleted,
                                   DataQuality quality)
{
    DeviceSnapshotData d;
    d.captureStarted = captureStarted;
    d.captureCompleted = captureCompleted;
    d.sequence = sequence;
    d.connected = connected;
    d.dataAgeMs = dataAgeMs;
    d.fast_quality = quality;
    d.fast_age_ms = dataAgeMs;

    // D100-D105 raw status words (indices 0,2,3,4,5).
    d.statusWord1 = raw[0];
    d.statusWord2 = raw[2];
    d.statusWord3 = raw[3];
    d.statusWord4 = raw[4];
    d.statusWord5 = raw[5];

    // D110 fault code, D120 step, D122 belt speed.
    d.faultCode = raw[10];
    d.currentStep = raw[20];
    d.beltSpeed = raw[22];

    // D126 low + D127 high -> uint32 width frequency.
    d.widthFrequency = decode::u32(raw[26], raw[27]);

    // D128 target width, D130 current width.
    d.targetWidth = raw[28];
    d.currentWidth = raw[30];

    // D136 low + D137 high -> int32 pulse count.
    d.pulseCount = decode::i32(raw[36], raw[37]);

    // D138 low + D139 high -> uint32 production count.
    d.productionCount = decode::u32(raw[38], raw[39]);

    // D140 heartbeat.
    d.heartbeat = raw[40];

    // Range checks (spec §9): out-of-range fields are marked invalid.
    checkRange(d, SnapshotField::FaultCode, d.faultCode, 0, 10);
    checkRange(d, SnapshotField::CurrentStep, d.currentStep, 0, 5);
    checkRange(d, SnapshotField::BeltSpeed, d.beltSpeed, 100, 20000);
    // D128 (目标宽度) is NOT range-checked (user decision 2026-09-21): the
    // former 50-400 check assumed integer millimetres and an operator envelope
    // the PLC does not implement. The registers now carry 0.1 mm units, the
    // upper bound is retired, and the "must be greater than zero" rule lives
    // at the operator entry gate (InterlockRules::checkAdjustWidth and the
    // width editor), never in the decode path: an out-of-range decoded field
    // would mark the whole snapshot OutOfRange and disable every
    // snapshotFresh()-gated control, which is the failure mode the D130
    // decision already removed. The SnapshotField::TargetWidth bit stays
    // defined so fieldValid() callers and the stored bit layout are unchanged;
    // it is simply never set here.
    // D130 (当前宽度) is likewise NOT range-checked: the PLC legitimately
    // reports 0 before the first homing/adjustment (MAIN first-scan init and
    // SBR_HOME's DMOV K0 D130). The SnapshotField::CurrentWidth bit stays
    // defined but is never set.
    checkRange(d, SnapshotField::Heartbeat, d.heartbeat, 0, 0xFFFF);

    d.overall_quality = aggregateQuality(d);
    return d;
}

void checkSlowBlockRange(DeviceSnapshotData &d)
{
    // Slow-block ranges (spec §9, requirement table): D204 1-32767, D220 1-15.
    // Clear the previous slow-block flags first so a later in-range slow poll
    // can restore the fields to valid.
    const quint32 slowMask = (quint32(1) << quint8(SnapshotField::PulsePerMm))
        | (quint32(1) << quint8(SnapshotField::WidthSpeed));
    d.invalidFields &= ~slowMask;
    checkRange(d, SnapshotField::PulsePerMm, d.pulsePerMm, 1, 32767);
    checkRange(d, SnapshotField::WidthSpeed, d.widthSpeed, 1, 15);
    // D210 has its own validity metadata (never aliases CurrentWidth).
    d.width_delta_valid = d.slow_quality == DataQuality::Valid;
    d.overall_quality = aggregateQuality(d);
}

DeviceSnapshot::DeviceSnapshot(const DeviceSnapshotData &d)
    : fast_quality(d.fast_quality)
    , fast_age_ms(d.fast_age_ms)
    , home_quality(d.home_quality)
    , home_age_ms(d.home_age_ms)
    , command_quality(d.command_quality)
    , command_age_ms(d.command_age_ms)
    , slow_quality(d.slow_quality)
    , slow_age_ms(d.slow_age_ms)
    , overall_quality(aggregateQuality(d))
    , overall_age_ms(std::max(
          {d.fast_age_ms, d.home_age_ms, d.command_age_ms, d.slow_age_ms}))
    , width_delta_valid(d.slow_quality == DataQuality::Valid)
    , m_captureStarted(d.captureStarted)
    , m_captureCompleted(d.captureCompleted)
    , m_sequence(d.sequence)
    , m_connected(d.connected)
    , m_dataAgeMs(d.dataAgeMs)
    , m_overall_quality(aggregateQuality(d))
    , m_statusWord1(d.statusWord1)
    , m_statusWord2(d.statusWord2)
    , m_statusWord3(d.statusWord3)
    , m_statusWord4(d.statusWord4)
    , m_statusWord5(d.statusWord5)
    , m_m0(statusBitFromEitherSource(d, 0))
    , m_m1(statusBitFromEitherSource(d, 1))
    , m_m2(statusBitFromEitherSource(d, 2))
    , m_m3(statusBitFromEitherSource(d, 3))
    , m_m4(statusBitFromEitherSource(d, 4))
    , m_m5(statusBitFromEitherSource(d, 5))
    , m_m6(statusBitFromEitherSource(d, 6))
    , m_m7(statusBitFromEitherSource(d, 7))
    , m_m8(statusBitFromEitherSource(d, 8))
    , m_m9(statusBitFromEitherSource(d, 9))
    , m_m10(statusBitFromEitherSource(d, 10))
    , m_m11(statusBitFromEitherSource(d, 11))
    , m_m12(statusBitFromEitherSource(d, 12))
    , m_m13(statusBitFromEitherSource(d, 13))
    , m_m14(statusBitFromEitherSource(d, 14))
    , m_m30(statusBit3FromEitherSource(d, 0))
    , m_m31(statusBit3FromEitherSource(d, 1))
    , m_m32(statusBit3FromEitherSource(d, 2))
    , m_m33(statusBit3FromEitherSource(d, 3))
    , m_m34(statusBit3FromEitherSource(d, 4))
    , m_m35(statusBit3FromEitherSource(d, 5))
    , m_m40(statusBit3FromEitherSource(d, 10))
    , m_m41(statusBit3FromEitherSource(d, 11))
    , m_m42(statusBit3FromEitherSource(d, 12))
    , m_m43(statusBit3FromEitherSource(d, 13))
    , m_m44(statusBit3FromEitherSource(d, 14))
    , m_m45(statusBit3FromEitherSource(d, 15))
    , m_faultCode(d.faultCode)
    , m_fault(FaultCodeTable::instance().info(d.faultCode))
    , m_currentStep(d.currentStep)
    , m_beltSpeed(d.beltSpeed)
    , m_widthFrequency(d.widthFrequency)
    , m_targetWidth(d.targetWidth)
    , m_currentWidth(d.currentWidth)
    , m_pulseCount(d.pulseCount)
    , m_productionCount(d.productionCount)
    , m_heartbeat(d.heartbeat)
    , m_pulsePerMm(d.pulsePerMm)
    , m_widthDelta(d.widthDelta)
    , m_widthSpeed(d.widthSpeed)
    , m_m50((d.homeBits & 0x01) != 0)
    , m_m51((d.homeBits & 0x02) != 0)
    , m_m52((d.homeBits & 0x04) != 0)
    , m_m53((d.homeBits & 0x08) != 0)
    , m_scanComplete(d.scanComplete)
    , m_m100((d.commandBits & 0x0001) != 0)
    , m_m101((d.commandBits & 0x0002) != 0)
    , m_m102((d.commandBits & 0x0004) != 0)
    , m_m103((d.commandBits & 0x0008) != 0)
    , m_m104((d.commandBits & 0x0010) != 0)
    , m_m105((d.commandBits & 0x0020) != 0)
    , m_m106((d.commandBits & 0x0040) != 0)
    , m_m107((d.commandBits & 0x0080) != 0)
    , m_m108((d.commandBits & 0x0100) != 0)
    , m_m109((d.commandBits & 0x0200) != 0)
    , m_m110((d.commandBits & 0x0400) != 0)
    , m_m111((d.commandBits & 0x0800) != 0)
    , m_invalidFields(d.invalidFields)
{
}

} // namespace hlm
