#include "application/interlock_rules.h"

namespace hlm {

namespace {

// M60/M61 are exposed via their D100-mapped bits M8/M9 (spec §8.2).
bool autoReady(const DeviceSnapshot &s) { return s.m8(); }
bool homed(const DeviceSnapshot &s) { return s.m9(); }

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

InterlockResult InterlockRules::checkAdjustWidth(const DeviceSnapshot &s, bool online,
                                                 quint16 targetWidth)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, !s.m34(), QStringLiteral("正在调宽, 请等待完成"));
    add(r.unmet, s.m1(), QStringLiteral("需要手动模式"));
    add(r.unmet, homed(s), QStringLiteral("未回原点"));
    add(r.unmet, !s.m3(), QStringLiteral("设备正在运行, 请先停止"));
    add(r.unmet, !s.m0(), QStringLiteral("急停有效"));
    add(r.unmet, !s.m14(), QStringLiteral("存在锁存故障"));
    add(r.unmet, !s.m50(), QStringLiteral("正在回原点"));
    add(r.unmet, targetWidth >= 50 && targetWidth <= 400,
        QStringLiteral("目标宽度需在 50-400 mm 之间"));
    add(r.unmet, s.fieldValid(SnapshotField::PulsePerMm) && s.pulsePerMm() >= 1
            && s.pulsePerMm() <= 32767,
        QStringLiteral("脉冲当量 D204 需在 1-32767 之间"));
    add(r.unmet, s.fieldValid(SnapshotField::WidthSpeed) && s.widthSpeed() >= 1
            && s.widthSpeed() <= 15,
        QStringLiteral("调宽速度 D220 需在 1-15 之间"));
    const quint32 freq = quint32(s.pulsePerMm()) * quint32(s.widthSpeed());
    add(r.unmet, freq >= 10 && freq <= 200000,
        QStringLiteral("D204×D220 需在 10-200000 之间"));
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

InterlockResult InterlockRules::checkManualCommand(const DeviceSnapshot &s, bool online)
{
    InterlockResult r;
    add(r.unmet, online, QStringLiteral("通讯中断"));
    add(r.unmet, s.m1(), QStringLiteral("需要手动模式"));
    add(r.unmet, homed(s), QStringLiteral("未回原点"));
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

} // namespace hlm
