// Task 2 unit tests: centralized address table, fault codes, data quality
// and immutable DeviceSnapshot decoding (spec §8.2, §9).
//
// Coverage required by the task brief:
// - D100/D103 bit mapping (M0-M14, M30-M45; M34/M42/M43/M44/M45 bits).
// - D126/D127, D136/D137, D138/D139 word order (low word first).
// - D210 signed negative (int16).
// - D140 wrap-around activity (value change, no monotonic requirement).
// - Out-of-range values mark the affected field invalid.
// - Unknown fault codes never crash (unknown latched fault).

#include <QtTest>

#include "domain/address_table.h"
#include "domain/device_snapshot.h"
#include "domain/fault_code.h"
#include "domain/quality.h"

using namespace hlm;

class SnapshotDecodeTest : public QObject
{
    Q_OBJECT

private slots:
    void d100BitMapping();
    void d103BitMapping();
    void d126d127WordOrder();
    void d136d137WordOrder();
    void d138d139WordOrder();
    void d210SignedNegative();
    void d140WrapActivity();
    void outOfRangeMarksFieldInvalid();
    void aggregateQualityWorstWins();
    void unknownFaultCodeDoesNotCrash();
    void addressTableLookup();
    void snapshotIsImmutable();
};

void SnapshotDecodeTest::d100BitMapping()
{
    // D100 bit0-bit14 -> M0-M14, bit15 reserved (spec §8.2).
    DeviceSnapshotData d;
    d.statusWord1 = 0x7FFF; // bits 0..14 set
    DeviceSnapshot s(d);
    QVERIFY(s.m0());
    QVERIFY(s.m1());
    QVERIFY(s.m2());
    QVERIFY(s.m3());
    QVERIFY(s.m4());
    QVERIFY(s.m5());
    QVERIFY(s.m6());
    QVERIFY(s.m7());
    QVERIFY(s.m8());
    QVERIFY(s.m9());
    QVERIFY(s.m10());
    QVERIFY(s.m11());
    QVERIFY(s.m12());
    QVERIFY(s.m13());
    QVERIFY(s.m14());
    QCOMPARE(s.statusWord1(), quint16(0x7FFF));

    DeviceSnapshotData d2;
    d2.statusWord1 = 0x0001;
    DeviceSnapshot s2(d2);
    QVERIFY(s2.m0());
    QVERIFY(!s2.m1());
    QVERIFY(!s2.m14());

    // decode helper agrees; bit15 is reserved and never reported.
    QVERIFY(decode::d100Bit(0x0004, 2));
    QVERIFY(!decode::d100Bit(0x0004, 3));
    QVERIFY(!decode::d100Bit(0xFFFF, 15));
}

void SnapshotDecodeTest::d103BitMapping()
{
    // D103 bit0-bit15 -> M30-M45; M34=bit4, M42=bit12, M43=bit13,
    // M44=bit14, M45=bit15 (spec §8.2).
    DeviceSnapshotData d;
    d.statusWord3 = 0xFFFF;
    DeviceSnapshot s(d);
    QVERIFY(s.m30());
    QVERIFY(s.m31());
    QVERIFY(s.m32());
    QVERIFY(s.m33());
    QVERIFY(s.m34());
    QVERIFY(s.m35());
    QVERIFY(s.m40());
    QVERIFY(s.m41());
    QVERIFY(s.m42());
    QVERIFY(s.m43());
    QVERIFY(s.m44());
    QVERIFY(s.m45());

    DeviceSnapshotData d2;
    d2.statusWord3 = (1u << 4) | (1u << 12) | (1u << 13) | (1u << 14) | (1u << 15);
    DeviceSnapshot s2(d2);
    QVERIFY(s2.m34());
    QVERIFY(s2.m42());
    QVERIFY(s2.m43());
    QVERIFY(s2.m44());
    QVERIFY(s2.m45());
    QVERIFY(!s2.m30());
    QVERIFY(!s2.m33());
    QVERIFY(!s2.m41());

    QVERIFY(decode::d103Bit(0x0010, 4));
    QVERIFY(!decode::d103Bit(0x0010, 5));
}

void SnapshotDecodeTest::d126d127WordOrder()
{
    // D126 low word, D127 high word, combined as uint32 (spec §8.2).
    QCOMPARE(decode::u32(0x1234, 0x5678), quint32(0x56781234));
    QCOMPARE(decode::u32(0x0000, 0x0001), quint32(0x00010000));
    QCOMPARE(decode::u32(0xFFFF, 0xFFFF), quint32(0xFFFFFFFF));

    // Full decode path: D126=0x1234, D127=0x5678.
    quint16 raw[41] = {};
    raw[26] = 0x1234; // D126
    raw[27] = 0x5678; // D127
    DeviceSnapshotData d = decodeFastBlock(raw, 1, true, 0,
        QDateTime::currentDateTime(), QDateTime::currentDateTime(), DataQuality::Valid);
    DeviceSnapshot s(d);
    QCOMPARE(s.widthFrequency(), quint32(0x56781234));
}

void SnapshotDecodeTest::d136d137WordOrder()
{
    // D136 low word, D137 high word, combined as int32 (spec §8.2).
    QCOMPARE(decode::i32(0x0001, 0x0000), qint32(1));
    QCOMPARE(decode::i32(0x0000, 0xFFFF), qint32(-65536));
    QCOMPARE(decode::i32(0xFFFF, 0xFFFF), qint32(-1));
    QCOMPARE(decode::i32(0x0000, 0x8000), qint32(-2147483647 - 1));

    quint16 raw[41] = {};
    raw[36] = 0x0000; // D136
    raw[37] = 0xFFFF; // D137 -> -65536
    DeviceSnapshotData d = decodeFastBlock(raw, 1, true, 0,
        QDateTime::currentDateTime(), QDateTime::currentDateTime(), DataQuality::Valid);
    DeviceSnapshot s(d);
    QCOMPARE(s.pulseCount(), qint32(-65536));
}

void SnapshotDecodeTest::d138d139WordOrder()
{
    // D138 low word, D139 high word, combined as uint32 (spec §8.2).
    QCOMPARE(decode::u32(0x0001, 0x0000), quint32(1));
    QCOMPARE(decode::u32(0x5678, 0x1234), quint32(0x12345678));
    QCOMPARE(decode::u32(0xFFFF, 0xFFFF), quint32(0xFFFFFFFF));

    quint16 raw[41] = {};
    raw[38] = 0xFFFF; // D138
    raw[39] = 0xFFFF; // D139
    DeviceSnapshotData d = decodeFastBlock(raw, 1, true, 0,
        QDateTime::currentDateTime(), QDateTime::currentDateTime(), DataQuality::Valid);
    DeviceSnapshot s(d);
    QCOMPARE(s.productionCount(), quint32(0xFFFFFFFF));
}

void SnapshotDecodeTest::d210SignedNegative()
{
    // D210 is int16 (target - current), may be negative (spec §8.2).
    QCOMPARE(decode::i16(0xFFFB), qint16(-5));
    QCOMPARE(decode::i16(0x0005), qint16(5));

    DeviceSnapshotData d;
    d.widthDelta = decode::i16(0xFFFB);
    DeviceSnapshot s(d);
    QCOMPARE(s.widthDelta(), qint16(-5));
}

void SnapshotDecodeTest::d140WrapActivity()
{
    // D140 activity = value changed; no monotonic requirement, 16-bit wrap OK.
    QVERIFY(decode::heartbeatActive(0xFFFF, 0x0000));
    QVERIFY(decode::heartbeatActive(0x0000, 0x0001));
    QVERIFY(decode::heartbeatActive(0x7FFF, 0x8000));
    QVERIFY(!decode::heartbeatActive(0x1234, 0x1234));

    DeviceSnapshotData d;
    d.heartbeat = 0xFFFF;
    DeviceSnapshot s(d);
    QCOMPARE(s.heartbeat(), quint16(0xFFFF));
}

void SnapshotDecodeTest::outOfRangeMarksFieldInvalid()
{
    // Out-of-range values mark the field invalid (spec §9, requirement table).
    quint16 raw[41] = {};
    raw[20] = 99;  // D120 current step: range 0-5
    raw[22] = 50;  // D122 belt speed: range 100-20000
    raw[28] = 10;  // D128 target width: range 50-400
    raw[30] = 500; // D130 current width: range 50-400
    raw[40] = 1;   // D140 heartbeat: no range

    DeviceSnapshotData d = decodeFastBlock(raw, 1, true, 0,
        QDateTime::currentDateTime(), QDateTime::currentDateTime(), DataQuality::Valid);
    DeviceSnapshot s(d);
    QVERIFY(!s.fieldValid(SnapshotField::CurrentStep));
    QVERIFY(!s.fieldValid(SnapshotField::BeltSpeed));
    QVERIFY(!s.fieldValid(SnapshotField::TargetWidth));
    QVERIFY(!s.fieldValid(SnapshotField::CurrentWidth));
    QVERIFY(s.fieldValid(SnapshotField::Heartbeat));
    QVERIFY(s.fieldValid(SnapshotField::FaultCode));
    QVERIFY(s.overallQuality() == DataQuality::OutOfRange);

    // In-range values stay valid.
    quint16 ok[41] = {};
    ok[20] = 3;
    ok[22] = 1000;
    ok[28] = 200;
    ok[30] = 200;
    ok[40] = 5;
    DeviceSnapshotData d2 = decodeFastBlock(ok, 2, true, 0,
        QDateTime::currentDateTime(), QDateTime::currentDateTime(), DataQuality::Valid);
    DeviceSnapshot s2(d2);
    QVERIFY(s2.fieldValid(SnapshotField::CurrentStep));
    QVERIFY(s2.fieldValid(SnapshotField::BeltSpeed));
    QVERIFY(s2.fieldValid(SnapshotField::TargetWidth));
    QVERIFY(s2.fieldValid(SnapshotField::CurrentWidth));
    QCOMPARE(s2.currentStep(), quint16(3));
    QCOMPARE(s2.beltSpeed(), quint16(1000));
    QCOMPARE(s2.targetWidth(), quint16(200));
}

void SnapshotDecodeTest::aggregateQualityWorstWins()
{
    DeviceSnapshotData d;
    d.fastQuality = DataQuality::Valid;
    d.homeQuality = DataQuality::Stale;
    d.commandQuality = DataQuality::Valid;
    d.slowQuality = DataQuality::Valid;
    QVERIFY(aggregateQuality(d) == DataQuality::Stale);

    d.slowQuality = DataQuality::ProtocolError;
    QVERIFY(aggregateQuality(d) == DataQuality::ProtocolError);

    d.slowQuality = DataQuality::Valid;
    d.invalidFields = (quint32(1) << quint8(SnapshotField::TargetWidth));
    QVERIFY(aggregateQuality(d) == DataQuality::OutOfRange);
}

void SnapshotDecodeTest::unknownFaultCodeDoesNotCrash()
{
    const FaultCodeTable &table = FaultCodeTable::instance();
    for (quint16 code : {quint16(0), quint16(1), quint16(5), quint16(9), quint16(10),
                         quint16(11), quint16(99), quint16(0xFFFF)}) {
        const FaultInfo f = table.info(code);
        QVERIFY(!f.meaning.isEmpty());
    }
    QCOMPARE(table.info(0).code, quint16(0));
    QCOMPARE(table.info(10).code, quint16(10));
    QVERIFY(table.info(10).meaning.contains(QStringLiteral("调宽定位超时")));
    QVERIFY(table.info(10).isLatched);
    QCOMPARE(table.info(99).code, quint16(99)); // unknown code preserved
    QVERIFY(table.info(99).isLatched);

    // info() returns a copy: a later query for a different unknown code must
    // not overwrite a previously returned value (thread-safety guarantee).
    const FaultInfo first = table.info(0x1111);
    const FaultInfo second = table.info(0x2222);
    QCOMPARE(first.code, quint16(0x1111));
    QCOMPARE(second.code, quint16(0x2222));
    QVERIFY(first.isLatched && second.isLatched);

    // Snapshot with an unknown fault code must not crash.
    DeviceSnapshotData d;
    d.faultCode = 99;
    DeviceSnapshot s(d);
    QCOMPARE(s.fault().code, quint16(99));
    QVERIFY(s.fault().isLatched);
}

void SnapshotDecodeTest::addressTableLookup()
{
    const AddressTable &t = AddressTable::instance();
    const AddressDef *d100 = t.find(QStringLiteral("D"), 100);
    QVERIFY(d100 != nullptr);
    QCOMPARE(d100->name, QStringLiteral("D100"));
    QVERIFY(d100->access == AccessType::Read);
    QVERIFY(d100->valueType == ValueType::U16);

    const AddressDef *m42 = t.find(QStringLiteral("M"), 42);
    QVERIFY(m42 != nullptr);
    QVERIFY(m42->access == AccessType::ReadWrite);

    const AddressDef *d126 = t.find(QStringLiteral("D"), 126);
    QVERIFY(d126 != nullptr);
    QVERIFY(d126->valueType == ValueType::U32);
    QCOMPARE(d126->highWord, quint16(127));

    const AddressDef *d136 = t.find(QStringLiteral("D"), 136);
    QVERIFY(d136 != nullptr);
    QVERIFY(d136->valueType == ValueType::I32);
    QCOMPARE(d136->highWord, quint16(137));

    const AddressDef *d138 = t.find(QStringLiteral("D"), 138);
    QVERIFY(d138 != nullptr);
    QVERIFY(d138->valueType == ValueType::U32);
    QCOMPARE(d138->highWord, quint16(139));

    const AddressDef *d210 = t.find(QStringLiteral("D"), 210);
    QVERIFY(d210 != nullptr);
    QVERIFY(d210->valueType == ValueType::I16);

    QVERIFY(t.find(QStringLiteral("D"), 999) == nullptr);
    QVERIFY(t.find(QStringLiteral("X"), 0) == nullptr);
    QVERIFY(t.findByName(QStringLiteral("D140")) != nullptr);
    QVERIFY(t.findByName(QStringLiteral("NOPE")) == nullptr);
}

void SnapshotDecodeTest::snapshotIsImmutable()
{
    // DeviceSnapshot is constructed atomically and exposes only const
    // accessors; values round-trip unchanged.
    DeviceSnapshotData d;
    d.sequence = 7;
    d.connected = true;
    d.dataAgeMs = 250;
    d.statusWord1 = 0x0001;
    d.faultCode = 3;
    d.currentStep = 2;
    d.beltSpeed = 1000;
    d.widthFrequency = 0x00020001;
    d.targetWidth = 200;
    d.currentWidth = 150;
    d.widthDelta = -50;
    d.pulseCount = -12345;
    d.productionCount = 999;
    d.heartbeat = 42;
    d.pulsePerMm = 1280;
    d.widthSpeed = 2;
    d.homeBits = 0x0F;
    d.commandBits = 0x1FFF;
    d.fastQuality = DataQuality::Valid;
    d.homeQuality = DataQuality::Valid;
    d.commandQuality = DataQuality::Valid;
    d.slowQuality = DataQuality::Valid;
    d.overallQuality = aggregateQuality(d);

    DeviceSnapshot s(d);
    QCOMPARE(s.sequence(), quint64(7));
    QVERIFY(s.connected());
    QCOMPARE(s.dataAgeMs(), qint64(250));
    QVERIFY(s.m0());
    QCOMPARE(s.faultCode(), quint16(3));
    QCOMPARE(s.currentStep(), quint16(2));
    QCOMPARE(s.beltSpeed(), quint16(1000));
    QCOMPARE(s.widthFrequency(), quint32(0x00020001));
    QCOMPARE(s.targetWidth(), quint16(200));
    QCOMPARE(s.currentWidth(), quint16(150));
    QCOMPARE(s.widthDelta(), qint16(-50));
    QCOMPARE(s.pulseCount(), qint32(-12345));
    QCOMPARE(s.productionCount(), quint32(999));
    QCOMPARE(s.heartbeat(), quint16(42));
    QCOMPARE(s.pulsePerMm(), quint16(1280));
    QCOMPARE(s.widthSpeed(), quint16(2));
    QVERIFY(s.m50() && s.m51() && s.m52() && s.m53());
    QVERIFY(s.m100() && s.m112());
    QVERIFY(s.overallQuality() == DataQuality::Valid);
}

QTEST_GUILESS_MAIN(SnapshotDecodeTest)
#include "test_snapshot_decode.moc"
