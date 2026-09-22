// Task 7 unit tests: per-command interlock precondition rules (spec §10).
// Pure functions over a DeviceSnapshot; no I/O.

#include <QtTest>

#include "application/interlock_rules.h"

using namespace hlm;

namespace {

// Build a snapshot with the given D100/D103 status words and slow-block values.
DeviceSnapshot makeSnapshot(quint16 statusWord1, quint16 statusWord3,
                            quint16 targetWidth = 200, quint16 currentWidth = 200,
                            quint16 pulsePerMm = 1280, quint16 widthSpeed = 15)
{
    DeviceSnapshotData d;
    d.statusWord1 = statusWord1;
    d.statusWord3 = statusWord3;
    d.targetWidth = targetWidth;
    d.currentWidth = currentWidth;
    d.pulsePerMm = pulsePerMm;
    d.widthSpeed = widthSpeed;
    d.overall_quality = aggregateQuality(d);
    return DeviceSnapshot(d);
}

// Bit helpers: D100 bit0-14 -> M0-M14; D103 bit0-15 -> M30-M45.
quint16 bit(quint16 word, int b) { return word | (quint16(1) << b); }

// A ready manual-mode snapshot: homed (M61 via M9), manual (M1), not running,
// no estop, no latched fault, not homing.
DeviceSnapshot readyManual()
{
    quint16 w1 = 0;
    w1 = bit(w1, 1);  // M1 manual
    w1 = bit(w1, 9);  // M9 = M61 homed
    return makeSnapshot(w1, 0);
}

} // namespace

class InterlockRulesTest : public QObject
{
    Q_OBJECT

private slots:
    void resetPreconditions();
    void homeStartPreconditions();
    void adjustWidthPreconditions();
    void adjustWidthRangeChecks();
    void startPreconditions();
    void stopPreconditions();
    void estopPreconditions();
    void modeSwitchPreconditions();
    void manualCommandPreconditions();
    void bypassPreconditions();
};

void InterlockRulesTest::resetPreconditions()
{
    // 前置: 在线、管理员(permission, not here)、M3=0.
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkReset(s, true).allowed);
    QVERIFY(!InterlockRules::checkReset(s, false).allowed); // offline

    // M3=1 running: denied.
    quint16 w1 = 0;
    w1 = bit(w1, 1);
    w1 = bit(w1, 9);
    w1 = bit(w1, 3); // M3 running
    QVERIFY(!InterlockRules::checkReset(makeSnapshot(w1, 0), true).allowed);
}

void InterlockRulesTest::adjustWidthPreconditions()
{
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkAdjustWidth(s, true, 300).allowed);

    // M34=1 adjusting: denied.
    quint16 w3 = bit(0, 4); // M34
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), w3), true, 300).allowed);

    // Not manual (M1=0): denied.
    quint16 w1 = bit(0, 9); // homed but not manual
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(w1, 0), true, 300).allowed);

    // Not homed (M9=0): ALLOWED (user decision 2026-09-21: 调宽 no longer
    // requires 回原点完成 M61; the operator starts homing with the 回原点
    // control, and the PLC ladder's own M61 precondition is what actually
    // refuses an un-homed M43 run).
    quint16 w1b = bit(0, 1);
    QVERIFY(InterlockRules::checkAdjustWidth(makeSnapshot(w1b, 0), true, 300).allowed);

    // Running M3=1: denied.
    quint16 w1c = bit(bit(bit(0,1),9),3);
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(w1c, 0), true, 300).allowed);

    // Estop M0=1: denied.
    quint16 w1d = bit(bit(bit(0,1),9),0);
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(w1d, 0), true, 300).allowed);

    // Latched fault M14=1: denied.
    quint16 w1e = bit(bit(bit(0,1),9),14);
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(w1e, 0), true, 300).allowed);

    // Homing M50=1: ALLOWED (user decision 2026-09-21: the homing-in-progress
    // bit is no longer an HMI 调宽 gate either).
    DeviceSnapshotData d;
    d.statusWord1 = bit(bit(0,1),9);
    d.homeBits = 0x01; // M50
    d.targetWidth = 200;
    d.currentWidth = 200;
    d.pulsePerMm = 1280;
    d.widthSpeed = 15;
    QVERIFY(InterlockRules::checkAdjustWidth(DeviceSnapshot(d), true, 300).allowed);

    // Offline: denied.
    QVERIFY(!InterlockRules::checkAdjustWidth(s, false, 300).allowed);
}

void InterlockRulesTest::adjustWidthRangeChecks()
{
    const DeviceSnapshot s = readyManual();
    // The retired 50-400 mm envelope is replaced by "greater than zero"
    // (user decision 2026-09-21): raw 0 is rejected, and everything above it
    // is accepted up to the u16 register ceiling.
    QVERIFY(!InterlockRules::checkAdjustWidth(s, true, 0).allowed);
    QVERIFY(InterlockRules::checkAdjustWidth(s, true, 1).allowed);
    QVERIFY(InterlockRules::checkAdjustWidth(s, true, 49).allowed);
    QVERIFY(InterlockRules::checkAdjustWidth(s, true, 401).allowed);
    QVERIFY(InterlockRules::checkAdjustWidth(s, true, 65535).allowed);

    // D204 out of 1-32767.
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), 0, 300, 200, 0, 15), true, 300).allowed);
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), 0, 300, 200, 32768, 15), true, 300).allowed);

    // D220 out of 1-15.
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), 0, 300, 200, 1280, 0), true, 300).allowed);
    QVERIFY(!InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), 0, 300, 200, 1280, 16), true, 300).allowed);

    // PLC-HMI-005 D4: the obsolete D204*D220 10-200000 frequency product is
    // not an interlock condition. Each field is validated against its own
    // decoded range, so extreme products with in-range fields are allowed.
    QVERIFY(InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), 0, 300, 200, 1, 1), true, 300).allowed);
    QVERIFY(InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), 0, 300, 200, 32767, 15), true, 300).allowed);
    QVERIFY(InterlockRules::checkAdjustWidth(makeSnapshot(bit(bit(0,1),9), 0, 300, 200, 1280, 15), true, 300).allowed);
}

void InterlockRulesTest::startPreconditions()
{
    // 前置: 在线、自动 M2=1、自动准备 M60(M8)=1、无急停、无锁存故障、未运行.
    quint16 w1 = bit(bit(bit(0,2),8),9); // M2 auto, M8=M60 ready, M9=M61 homed
    QVERIFY(InterlockRules::checkStart(makeSnapshot(w1, 0), true).allowed);

    // Not auto (M2=0): denied.
    quint16 w1b = bit(bit(0,8),9);
    QVERIFY(!InterlockRules::checkStart(makeSnapshot(w1b, 0), true).allowed);

    // Not ready (M8=0): denied.
    quint16 w1c = bit(bit(0,2),9);
    QVERIFY(!InterlockRules::checkStart(makeSnapshot(w1c, 0), true).allowed);

    // Estop M0=1: denied.
    quint16 w1d = bit(bit(bit(bit(0,2),8),9),0);
    QVERIFY(!InterlockRules::checkStart(makeSnapshot(w1d, 0), true).allowed);

    // Latched fault M14=1: denied.
    quint16 w1e = bit(bit(bit(bit(0,2),8),9),14);
    QVERIFY(!InterlockRules::checkStart(makeSnapshot(w1e, 0), true).allowed);

    // Running M3=1: still allowed (user decision 2026-09-22). The decoded PLC
    // start rung does not require M3=0 — M3 is a seal-in contact — so the HMI
    // must not be stricter than the machine; the PLC decides whether the run
    // starts, and a re-sent M101 pulse is harmless.
    quint16 w1f = bit(bit(bit(bit(0,2),8),9),3);
    QVERIFY(InterlockRules::checkStart(makeSnapshot(w1f, 0), true).allowed);
    QVERIFY(InterlockRules::checkStart(makeSnapshot(w1f, 0), true).unmet.isEmpty());

    // Offline: denied.
    QVERIFY(!InterlockRules::checkStart(makeSnapshot(w1, 0), false).allowed);
}

void InterlockRulesTest::stopPreconditions()
{
    // 在线时未登录/操作员/管理员均可停止 (permission, not here); 离线不尝试.
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkStop(s, true).allowed);
    QVERIFY(!InterlockRules::checkStop(s, false).allowed);
}

void InterlockRulesTest::estopPreconditions()
{
    // 置位: 在线即可, 不要求登录或确认 (spec §10.6).
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkEstopSet(s, true).allowed);
    QVERIFY(!InterlockRules::checkEstopSet(s, false).allowed);

    // 解除: 在线即可 (permission gates admin); 解除成功≠设备可运行.
    QVERIFY(InterlockRules::checkEstopRelease(s, true).allowed);
    QVERIFY(!InterlockRules::checkEstopRelease(s, false).allowed);
}

void InterlockRulesTest::modeSwitchPreconditions()
{
    // 独立模式切换: 在线、M3=0 (permission gates admin).
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkModeSwitch(s, true).allowed);
    QVERIFY(!InterlockRules::checkModeSwitch(s, false).allowed);

    // M3=1 running: 禁止切换.
    quint16 w1 = bit(bit(bit(0,1),9),3);
    QVERIFY(!InterlockRules::checkModeSwitch(makeSnapshot(w1, 0), true).allowed);
}

void InterlockRulesTest::manualCommandPreconditions()
{
    // 要求手动模式、M3=0、无急停无锁存故障 (permission gates admin).
    // User decision 2026-09-21: homing completion (M61/M9) and the
    // homing-in-progress bit (M50) are NOT manual gates for any of the four
    // manual commands.
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkManualCommand(s, true).allowed);
    QVERIFY(!InterlockRules::checkManualCommand(s, false).allowed);

    // Not manual.
    quint16 w1 = bit(0, 9);
    QVERIFY(!InterlockRules::checkManualCommand(makeSnapshot(w1, 0), true).allowed);

    // Running.
    quint16 w1b = bit(bit(bit(0,1),9),3);
    QVERIFY(!InterlockRules::checkManualCommand(makeSnapshot(w1b, 0), true).allowed);

    // Estop.
    quint16 w1c = bit(bit(bit(0,1),9),0);
    QVERIFY(!InterlockRules::checkManualCommand(makeSnapshot(w1c, 0), true).allowed);

    // Latched fault.
    quint16 w1d = bit(bit(bit(0,1),9),14);
    QVERIFY(!InterlockRules::checkManualCommand(makeSnapshot(w1d, 0), true).allowed);

    // Not homed (M9=0): ALLOWED for every manual address, including the width
    // jogs M106/M107 and the address-less historical entry.
    quint16 w1e = bit(0, 1); // manual, not homed
    const DeviceSnapshot notHomed = makeSnapshot(w1e, 0);
    QVERIFY(InterlockRules::checkManualCommand(notHomed, true, 106).allowed);
    QVERIFY(InterlockRules::checkManualCommand(notHomed, true, 107).allowed);
    QVERIFY(InterlockRules::checkManualCommand(notHomed, true, 108).allowed);
    QVERIFY(InterlockRules::checkManualCommand(notHomed, true, 109).allowed);
    QVERIFY(InterlockRules::checkManualCommand(notHomed, true).allowed);

    // Homing in progress (M50=1): also allowed; only the dedicated 回原点
    // command is refused while M50 reads high.
    DeviceSnapshotData homing;
    homing.statusWord1 = bit(0, 1); // manual
    homing.homeBits = 0x01;         // M50
    homing.targetWidth = 200;
    homing.currentWidth = 200;
    homing.pulsePerMm = 1280;
    homing.widthSpeed = 15;
    const DeviceSnapshot homingSnapshot(homing);
    QVERIFY(InterlockRules::checkManualCommand(homingSnapshot, true, 106).allowed);
    QVERIFY(InterlockRules::checkManualCommand(homingSnapshot, true, 108).allowed);
}

void InterlockRulesTest::homeStartPreconditions()
{
    // 回原点 (M50=1 持续写): 在线、手动模式、M3=0、无急停、无锁存故障、未在回原点
    // (permission gates admin).
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkHomeStart(s, true).allowed);
    QVERIFY(!InterlockRules::checkHomeStart(s, false).allowed);

    // Not manual.
    QVERIFY(!InterlockRules::checkHomeStart(makeSnapshot(bit(0, 9), 0), true).allowed);

    // Running.
    QVERIFY(!InterlockRules::checkHomeStart(makeSnapshot(bit(bit(bit(0,1),9),3), 0), true).allowed);

    // Estop.
    QVERIFY(!InterlockRules::checkHomeStart(makeSnapshot(bit(bit(bit(0,1),9),0), 0), true).allowed);

    // Latched fault.
    QVERIFY(!InterlockRules::checkHomeStart(makeSnapshot(bit(bit(bit(0,1),9),14), 0), true).allowed);

    // Already homing (M50=1): denied.
    DeviceSnapshotData homing;
    homing.statusWord1 = bit(bit(0,1),9);
    homing.homeBits = 0x01; // M50
    homing.targetWidth = 200;
    homing.currentWidth = 200;
    homing.pulsePerMm = 1280;
    homing.widthSpeed = 15;
    QVERIFY(!InterlockRules::checkHomeStart(DeviceSnapshot(homing), true).allowed);
}

void InterlockRulesTest::bypassPreconditions()
{
    // 屏蔽: 在线即可 (permission gates admin + audit).
    const DeviceSnapshot s = readyManual();
    QVERIFY(InterlockRules::checkBypass(s, true).allowed);
    QVERIFY(!InterlockRules::checkBypass(s, false).allowed);
}

QTEST_GUILESS_MAIN(InterlockRulesTest)
#include "test_interlock_rules.moc"
