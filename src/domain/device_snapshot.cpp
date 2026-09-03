#include "domain/device_snapshot.h"

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
    DataQuality q = d.fastQuality;
    q = worstQuality(q, d.homeQuality);
    q = worstQuality(q, d.commandQuality);
    q = worstQuality(q, d.slowQuality);
    if (d.invalidFields != 0)
        q = worstQuality(q, DataQuality::OutOfRange);
    return q;
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
    d.fastQuality = quality;

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

    d.overallQuality = aggregateQuality(d);
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
    d.overallQuality = aggregateQuality(d);
}

DeviceSnapshot::DeviceSnapshot(const DeviceSnapshotData &d)
    : m_captureStarted(d.captureStarted)
    , m_captureCompleted(d.captureCompleted)
    , m_sequence(d.sequence)
    , m_connected(d.connected)
    , m_dataAgeMs(d.dataAgeMs)
    , m_overallQuality(d.overallQuality)
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
    , m_m112((d.commandBits & 0x1000) != 0)
    , m_fastQuality(d.fastQuality)
    , m_homeQuality(d.homeQuality)
    , m_commandQuality(d.commandQuality)
    , m_slowQuality(d.slowQuality)
    , m_invalidFields(d.invalidFields)
{
}

} // namespace hlm
