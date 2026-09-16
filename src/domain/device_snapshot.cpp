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

} // namespace

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
    // WidthDelta validity requires a valid slow block and signed D210 in the
    // authoritative -350..350 range; it never aliases CurrentWidth validity.
    d.width_delta_valid = d.slow_quality == DataQuality::Valid
        && d.widthDelta >= -350 && d.widthDelta <= 350;
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
    checkRange(d, SnapshotField::TargetWidth, d.targetWidth, 50, 400);
    checkRange(d, SnapshotField::CurrentWidth, d.currentWidth, 50, 400);
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
    d.width_delta_valid = d.slow_quality == DataQuality::Valid
        && d.widthDelta >= -350 && d.widthDelta <= 350;
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
    , width_delta_valid(d.slow_quality == DataQuality::Valid
                        && d.widthDelta >= -350 && d.widthDelta <= 350)
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
    , m_m0(decode::d100Bit(d.statusWord1, 0))
    , m_m1(decode::d100Bit(d.statusWord1, 1))
    , m_m2(decode::d100Bit(d.statusWord1, 2))
    , m_m3(decode::d100Bit(d.statusWord1, 3))
    , m_m4(decode::d100Bit(d.statusWord1, 4))
    , m_m5(decode::d100Bit(d.statusWord1, 5))
    , m_m6(decode::d100Bit(d.statusWord1, 6))
    , m_m7(decode::d100Bit(d.statusWord1, 7))
    , m_m8(decode::d100Bit(d.statusWord1, 8))
    , m_m9(decode::d100Bit(d.statusWord1, 9))
    , m_m10(decode::d100Bit(d.statusWord1, 10))
    , m_m11(decode::d100Bit(d.statusWord1, 11))
    , m_m12(decode::d100Bit(d.statusWord1, 12))
    , m_m13(decode::d100Bit(d.statusWord1, 13))
    , m_m14(decode::d100Bit(d.statusWord1, 14))
    , m_m30(decode::d103Bit(d.statusWord3, 0))
    , m_m31(decode::d103Bit(d.statusWord3, 1))
    , m_m32(decode::d103Bit(d.statusWord3, 2))
    , m_m33(decode::d103Bit(d.statusWord3, 3))
    , m_m34(decode::d103Bit(d.statusWord3, 4))
    , m_m35(decode::d103Bit(d.statusWord3, 5))
    , m_m40(decode::d103Bit(d.statusWord3, 10))
    , m_m41(decode::d103Bit(d.statusWord3, 11))
    , m_m42(decode::d103Bit(d.statusWord3, 12))
    , m_m43(decode::d103Bit(d.statusWord3, 13))
    , m_m44(decode::d103Bit(d.statusWord3, 14))
    , m_m45(decode::d103Bit(d.statusWord3, 15))
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
