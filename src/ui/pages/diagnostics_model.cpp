#include "ui/pages/diagnostics_model.h"

#include "ui/shell/shell_model.h"

namespace hlm {

namespace {

// Defined bit names (需求/PLC上位机地址及要求.txt + 补充.txt).
const char *d100BitName(int m)
{
    switch (m) {
    case 0: return "急停有效";
    case 1: return "手动模式";
    case 2: return "自动模式";
    case 3: return "运行状态";
    case 4: return "实时故障";
    case 5: return "安全门打开";
    case 6: return "安全光栅遮挡";
    case 7: return "气压低";
    case 8: return "自动准备完成";
    case 9: return "回原点完成";
    case 10: return "自动挡停伸出";
    case 11: return "相机触发中";
    case 12: return "拍照超时标志";
    case 13: return "直通模式";
    case 14: return "故障锁存";
    default: return "";
    }
}

const char *d103BitName(int m)
{
    switch (m) {
    case 30: return "手动皮带点动";
    case 31: return "手动调宽正转";
    case 32: return "手动调宽反转";
    case 33: return "手动挡停";
    case 34: return "自动调宽运行";
    case 35: return "调宽方向";
    case 40: return "流程皮带运行";
    case 41: return "皮带输出汇总";
    case 42: return "皮带自动常转";
    case 43: return "调宽启动命令";
    case 44: return "调宽完成";
    case 45: return "调宽失败";
    default: return "";
    }
}

const char *homeCommandBitName(int m)
{
    switch (m) {
    case 50: return "回原点启动";
    case 51: return "回原点阶段1反转";
    case 52: return "回原点阶段2正转";
    case 53: return "回原点下降沿辅助";
    case 100: return "上位机急停请求";
    case 101: return "上位机启动";
    case 102: return "上位机停止";
    case 103: return "上位机复位";
    case 104: return "自动模式";
    case 105: return "直通模式";
    case 106: return "手动调宽正转";
    case 107: return "手动调宽反转";
    case 108: return "手动皮带点动";
    case 109: return "手动挡停";
    case 110: return "光栅屏蔽";
    case 111: return "门磁屏蔽";
    default: return "";
    }
}

// A source block is trusted only when linked and its quality is Valid. The
// adapters publish evidence-based quality/age (PLC-HMI-003 D6), so a failed
// or stale transfer blanks exactly the affected rows (spec §9).
bool blockFresh(const DeviceSnapshot &s, DataQuality quality)
{
    return s.connected() && quality == DataQuality::Valid;
}

} // namespace

DiagnosticsModel::DiagnosticsModel(const ShellModel &model)
    : m_model(model)
{
}

bool DiagnosticsModel::fastFresh() const
{
    const DeviceSnapshot &s = m_model.snapshot();
    return blockFresh(s, s.fastQuality());
}

bool DiagnosticsModel::slowFresh() const
{
    const DeviceSnapshot &s = m_model.snapshot();
    return blockFresh(s, s.slowQuality());
}

bool DiagnosticsModel::homeFresh() const
{
    const DeviceSnapshot &s = m_model.snapshot();
    return blockFresh(s, s.homeQuality());
}

bool DiagnosticsModel::commandFresh() const
{
    const DeviceSnapshot &s = m_model.snapshot();
    return blockFresh(s, s.commandQuality());
}

DiagnosticsField DiagnosticsModel::field(const QString &text, quint8 f,
                                         bool sourceFresh) const
{
    if (!sourceFresh || !m_model.snapshot().fieldValid(SnapshotField(f)))
        return {}; // invalid -> "—" (spec §9)
    return {text, true};
}

// --- raw status words D100-D105 ------------------------------------------------

bool DiagnosticsModel::rawWordsValid() const
{
    return fastFresh();
}

QString DiagnosticsModel::rawWordHex(int index) const
{
    if (!fastFresh())
        return QStringLiteral("—");
    const DeviceSnapshot &s = m_model.snapshot();
    quint16 value = 0;
    switch (index) {
    case 0: value = s.statusWord1(); break; // D100
    case 1: value = s.statusWord2(); break; // D102 (raw only)
    case 2: value = s.statusWord3(); break; // D103
    case 3: value = s.statusWord4(); break; // D104 (raw only)
    case 4: value = s.statusWord5(); break; // D105 (raw only)
    default: return QStringLiteral("—");
    }
    return QStringLiteral("0x%1")
        .arg(QString::number(value, 16).rightJustified(4, QLatin1Char('0')).toUpper());
}

// --- defined bits -----------------------------------------------------------------

bool DiagnosticsModel::bitValid() const
{
    // D100/D103 bits are decoded from the fast block.
    return fastFresh();
}

bool DiagnosticsModel::bitState(int mNumber) const
{
    const DeviceSnapshot &s = m_model.snapshot();
    if (mNumber >= 0 && mNumber <= 14)
        return fastFresh() && decode::d100Bit(s.statusWord1(), mNumber); // D100 -> M0-M14
    if (mNumber >= 30 && mNumber <= 45)
        return fastFresh() && decode::d103Bit(s.statusWord3(), mNumber - 30); // D103 -> M30-M45
    if (mNumber >= 50 && mNumber <= 53) {
        // M50-M53 (function code 01 readback, home block).
        if (!homeFresh())
            return false;
        switch (mNumber) {
        case 50: return s.m50();
        case 51: return s.m51();
        case 52: return s.m52();
        default: return s.m53();
        }
    }
    if (mNumber >= 100 && mNumber <= 111) {
        // M100-M111 command readback (function code 01, command block).
        // M112 was removed from the live path (PLC-HMI-003 D3).
        if (!commandFresh())
            return false;
        switch (mNumber) {
        case 100: return s.m100();
        case 101: return s.m101();
        case 102: return s.m102();
        case 103: return s.m103();
        case 104: return s.m104();
        case 105: return s.m105();
        case 106: return s.m106();
        case 107: return s.m107();
        case 108: return s.m108();
        case 109: return s.m109();
        case 110: return s.m110();
        default: return s.m111();
        }
    }
    return false; // undefined M (D102/D104/D105 bits never exposed)
}

QVector<BitRow> DiagnosticsModel::d100Bits() const
{
    QVector<BitRow> rows;
    for (int m = 0; m <= 14; ++m) {
        BitRow r;
        r.mNumber = m;
        r.name = QString::fromUtf8(d100BitName(m));
        r.known = bitValid();
        r.state = r.known && bitState(m);
        rows.append(r);
    }
    return rows;
}

QVector<BitRow> DiagnosticsModel::d103Bits() const
{
    QVector<BitRow> rows;
    const int defined[] = {30, 31, 32, 33, 34, 35, 40, 41, 42, 43, 44, 45};
    for (int m : defined) {
        BitRow r;
        r.mNumber = m;
        r.name = QString::fromUtf8(d103BitName(m));
        r.known = bitValid();
        r.state = r.known && bitState(m);
        rows.append(r);
    }
    return rows;
}

QVector<BitRow> DiagnosticsModel::homeCommandBits() const
{
    QVector<BitRow> rows;
    const int defined[] = {50, 51, 52, 53, 100, 101, 102, 103, 104,
                           105, 106, 107, 108, 109, 110, 111};
    for (int m : defined) {
        BitRow r;
        r.mNumber = m;
        r.name = QString::fromUtf8(homeCommandBitName(m));
        // M50-M53 come from the home block, M100-M111 from the command block.
        r.known = (m <= 53) ? homeFresh() : commandFresh();
        r.state = r.known && bitState(m);
        rows.append(r);
    }
    return rows;
}

// --- D140 heartbeat activity (spec §13) ------------------------------------------

bool DiagnosticsModel::heartbeatKnown() const
{
    // D140 lives in the fast block. Stale/offline fast data: activity is
    // unknown, show "—" (spec §9). Never record a baseline from an expired
    // snapshot, otherwise the light would stay "活性" forever after the PLC
    // disconnects (spec §13: D140 frozen).
    if (!fastFresh())
        return false;
    if (m_hasLastHeartbeat)
        return true;
    // First observed snapshot: record it as the activity baseline.
    const DeviceSnapshot &s = m_model.snapshot();
    m_lastHeartbeat = s.heartbeat();
    m_lastSequence = s.sequence();
    m_lastActive = true;
    m_hasLastHeartbeat = true;
    return true;
}

bool DiagnosticsModel::heartbeatActive() const
{
    if (!heartbeatKnown())
        return false;
    const DeviceSnapshot &s = m_model.snapshot();
    // Activity is judged between consecutive snapshots: a heartbeat change
    // means the PLC is alive. Non-snapshot state changes never reach here
    // because we only compare when the snapshot sequence advanced.
    if (s.sequence() == m_lastSequence)
        return m_lastActive;
    m_lastActive = (m_lastHeartbeat != s.heartbeat());
    m_lastHeartbeat = s.heartbeat();
    m_lastSequence = s.sequence();
    return m_lastActive;
}

// --- key registers -----------------------------------------------------------------

DiagnosticsField DiagnosticsModel::faultCode() const
{
    return field(QString::number(m_model.snapshot().faultCode()),
                 quint8(SnapshotField::FaultCode), fastFresh()); // D110
}

DiagnosticsField DiagnosticsModel::currentStep() const
{
    // D120 is in the fast block and has a defined 0-5 range.
    return field(QString::number(m_model.snapshot().currentStep()),
                 quint8(SnapshotField::CurrentStep), fastFresh());
}

DiagnosticsField DiagnosticsModel::beltSpeed() const
{
    return field(QString::number(m_model.snapshot().beltSpeed()),
                 quint8(SnapshotField::BeltSpeed), fastFresh()); // D122
}

DiagnosticsField DiagnosticsModel::widthFrequency() const
{
    if (!fastFresh())
        return {};
    return {QString::number(m_model.snapshot().widthFrequency()), true}; // D126/127
}

DiagnosticsField DiagnosticsModel::targetWidth() const
{
    return field(QString::number(m_model.snapshot().targetWidth()),
                 quint8(SnapshotField::TargetWidth), fastFresh()); // D128
}

DiagnosticsField DiagnosticsModel::currentWidth() const
{
    return field(QString::number(m_model.snapshot().currentWidth()),
                 quint8(SnapshotField::CurrentWidth), fastFresh()); // D130
}

DiagnosticsField DiagnosticsModel::pulseCount() const
{
    if (!fastFresh())
        return {};
    return {QString::number(m_model.snapshot().pulseCount()), true}; // D136/137
}

DiagnosticsField DiagnosticsModel::productionCount() const
{
    if (!fastFresh())
        return {};
    return {QString::number(m_model.snapshot().productionCount()), true}; // D138/139
}

QString DiagnosticsModel::productionCountReliabilityText() const
{
    return QStringLiteral("自动调宽后该计数可能不可靠（PLC DMUL 覆盖 D138/D139）");
}

DiagnosticsField DiagnosticsModel::pulsePerMm() const
{
    return field(QString::number(m_model.snapshot().pulsePerMm()),
                 quint8(SnapshotField::PulsePerMm), slowFresh()); // D204
}

DiagnosticsField DiagnosticsModel::widthDelta() const
{
    // D210 has its own validity metadata (never aliases CurrentWidth):
    // a valid slow block plus signed D210 in -350..350 (PLC-HMI-003 D6/D7).
    const DeviceSnapshot &s = m_model.snapshot();
    if (!slowFresh() || !s.width_delta_valid)
        return {};
    return {QString::number(s.widthDelta()), true}; // D210
}

DiagnosticsField DiagnosticsModel::widthSpeed() const
{
    return field(QString::number(m_model.snapshot().widthSpeed()),
                 quint8(SnapshotField::WidthSpeed), slowFresh()); // D220
}

// --- vision self-test (wired by Task 20) -------------------------------------------

void DiagnosticsModel::setVisionStatus(const QString &version, bool healthy,
                                       const QString &failureReason)
{
    m_visionVersion = version;
    m_visionHealthy = healthy;
    m_visionFailureReason = failureReason;
}

} // namespace hlm
