#include "application/interlock_rules.h"

namespace hlm {

namespace {

// M60 is exposed via its D100-mapped bit M8 (spec §8.2).
bool autoReady(const DeviceSnapshot &s) { return s.m8(); }

void add(QStringList &list, bool ok, const QString &reason)
{
    if (!ok)
        list.append(reason);
}

} // namespace

InterlockResult InterlockRules::checkReset(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, !s.m3(), QStringLiteral("设备正在运行, 请先停止"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkHomeStart(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, s.m1(), QStringLiteral("需要手动模式"));
    add(r.unmet, !s.m3(), QStringLiteral("设备正在运行, 请先停止"));
    add(r.unmet, !s.m0(), QStringLiteral("急停有效"));
    add(r.unmet, !s.m14(), QStringLiteral("存在锁存故障"));
    add(r.unmet, !s.m50(), QStringLiteral("正在回原点"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkAdjustWidth(const DeviceSnapshot &s, bool online,
                                                 quint16 targetWidth)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, !s.m34(), QStringLiteral("正在调宽, 请等待完成"));
    add(r.unmet, s.m1(), QStringLiteral("需要手动模式"));
    // 用户决定 (2026-09-21): 不再要求回原点完成 M61, 也不因 M50=0 放行.
    // 注意 PLC 梯级 SBR_MANUALWIDTH 的 M43 前置条件仍含 M61, 因此未回原点时
    // 调宽会在 PLC 侧被拒绝 (D128 已写入, M44=0/M45=1): 这是有意的 HMI/PLC
    // 行为差异, 由操作员通过回原点按钮消除.
    add(r.unmet, !s.m3(), QStringLiteral("设备正在运行, 请先停止"));
    add(r.unmet, !s.m0(), QStringLiteral("急停有效"));
    add(r.unmet, !s.m14(), QStringLiteral("存在锁存故障"));
    // 目标宽度必须是大于 0 的原始寄存器值 (用户决定 2026-09-21): 寄存器单位
    // 是 0.1mm, 旧的上限 400 (以及下限 50) 已随操作员包络一并取消. 该包络本来
    // 就只是 HMI 侧的限制, 不是解码 PLC 梯图得到的 (梯图对 D128/D130 无任何数值
    // 比较). 注意 PLC 侧依赖: 梯图用 D210 × D204 计算 DDRVI 距离, 因此 D204 必须
    // 是"每 0.1mm 的脉冲数"该单位才成立 —— 这属于 PLC 工程师的改动, 不是 HMI.
    add(r.unmet, targetWidth > 0, QStringLiteral("目标宽度需大于 0 mm"));
    add(r.unmet, s.fieldValid(SnapshotField::PulsePerMm) && s.pulsePerMm() >= 1
            && s.pulsePerMm() <= 32767,
        QStringLiteral("脉冲当量 D204 需在 1-32767 之间"));
    add(r.unmet, s.fieldValid(SnapshotField::WidthSpeed) && s.widthSpeed() >= 1
            && s.widthSpeed() <= 15,
        QStringLiteral("调宽速度 D220 需在 1-15 之间"));
    // No D204*D220 frequency-product interlock: the decoded PLC validates each
    // field independently (PLC-HMI-005 D4).
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkStart(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, s.m2(), QStringLiteral("需要自动模式"));
    add(r.unmet, autoReady(s), QStringLiteral("自动准备未完成"));
    add(r.unmet, !s.m0(), QStringLiteral("急停有效"));
    add(r.unmet, !s.m14(), QStringLiteral("存在锁存故障"));
    add(r.unmet, !s.m3(), QStringLiteral("设备正在运行"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkStop(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online,
        QStringLiteral("通讯中断, 命令无法送达, 请使用现场停止或实体急停"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkEstopSet(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断, 请使用实体急停"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkEstopRelease(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkModeSwitch(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, !s.m3(), QStringLiteral("设备正在运行, 请先停止并确认 M3=0"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkManualCommand(const DeviceSnapshot &s, bool online,
                                                   quint16 address)
{
    Q_UNUSED(address);
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, s.m1(), QStringLiteral("需要手动模式"));
    // User decision (2026-09-21): homing completion is no longer an HMI manual
    // gate for any manual control. The homed/in-homing verdict keeps its
    // independent home-start interlock (checkHomeStart), and the operator sees
    // M61 on the top bar. Note the PLC ladder still gates M106/M107 on M61
    // (SBR_MANUAL groups 4/7), so an un-homed width jog reaches the PLC and is
    // refused there; that HMI/PLC divergence is intentional and cleared by
    // pressing 回原点.
    add(r.unmet, !s.m3(), QStringLiteral("设备正在运行, 请先停止"));
    add(r.unmet, !s.m0(), QStringLiteral("急停有效"));
    add(r.unmet, !s.m14(), QStringLiteral("存在锁存故障"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkBypass(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

InterlockResult InterlockRules::checkSimStationSignal(const DeviceSnapshot &s, bool online)
{
    // Test-only signal injection (user decision 2026-09-22): online is the
    // only gate, because the signal has to be injectable while the flow runs.
    Q_UNUSED(s);
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    r.allowed = r.unmet.isEmpty();
    return r;
}

} // namespace hlm
