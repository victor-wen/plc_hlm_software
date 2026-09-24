#include "ui/shell/shell_model.h"

namespace hlm {

ShellModel::ShellModel(QObject *parent)
    : QObject(parent)
    , m_emptySnapshot(DeviceSnapshotData())
{
}

void ShellModel::updateSnapshot(const DeviceSnapshot &s)
{
    m_snapshot = s;
    m_online = s.connected();
    emit stateChanged();
}

void ShellModel::setOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    emit stateChanged();
}

void ShellModel::setUser(const QString &name, Role role)
{
    m_userName = name;
    m_role = role;
    emit userChanged();
    emit stateChanged();
}

void ShellModel::clearUser()
{
    setUser(QString(), Role::Anonymous);
}

void ShellModel::setCommandPending(Command cmd, bool pending)
{
    if (pending)
        m_pending.insert(int(cmd), true);
    else
        m_pending.remove(int(cmd));
    emit stateChanged();
}

void ShellModel::setScanInProgress(bool inProgress)
{
    if (m_scanInProgress == inProgress)
        return;
    m_scanInProgress = inProgress;
    emit stateChanged();
}

void ShellModel::setOperatorCommandStatus(const OperatorCommandStatus &status)
{
    m_operatorCommandStatus = status;
    emit operatorCommandStatusChanged(m_operatorCommandStatus);
    emit stateChanged();
}

void ShellModel::clearOperatorCommandStatus()
{
    m_operatorCommandStatus = OperatorCommandStatus();
    emit operatorCommandStatusChanged(m_operatorCommandStatus);
    emit stateChanged();
}

bool ShellModel::snapshotFresh() const
{
    if (!m_snapshot.has_value())
        return false;
    const DeviceSnapshot &s = *m_snapshot;
    // Any block stale/errored or the aggregate not Valid -> not fresh
    // (spec §9, §11.2: 过期/无效字段显示"—"并禁用依赖动作).
    return s.connected() && s.overallQuality() == DataQuality::Valid
        && s.fastQuality() == DataQuality::Valid
        && s.homeQuality() == DataQuality::Valid
        && s.commandQuality() == DataQuality::Valid
        && s.slowQuality() == DataQuality::Valid;
}

bool ShellModel::modeKnown() const
{
    // The mode/run/homed state bits (M1/M2/M3/M9) come from the fast status
    // block (D100, spec §8.2). Only that block's transport/age quality gates
    // whether they are known: an unrelated out-of-range field (e.g. D130=0)
    // must not blank the whole top bar (spec §9, §11.2). The adapters publish
    // real per-block quality/age (PLC-HMI-003 D6), so a stale or failed fast
    // block downgrades this gate instead of degenerating to connected().
    //
    // The link state is checked first (user decision 2026-09-24). The gateway
    // emits only connectionStateChanged(false) on a link loss — it never
    // publishes a disconnected snapshot — so the cached snapshot keeps its own
    // connected()==true and every verdict derived from it would otherwise
    // survive the outage and describe a machine nobody is talking to.
    if (!m_online)
        return false;
    const DeviceSnapshot &s = snapshot();
    return s.connected() && s.fastQuality() == DataQuality::Valid;
}

bool ShellModel::isAutoMode() const
{
    if (!modeKnown())
        return false;
    return snapshot().m2();
}

bool ShellModel::isRunning() const
{
    if (!modeKnown())
        return false;
    return snapshot().m3();
}

bool ShellModel::isHomed() const
{
    if (!modeKnown())
        return false;
    return snapshot().m9(); // M61 via D100 bit9 (spec §8.2)
}

bool ShellModel::isFaulted() const
{
    if (!modeKnown())
        return false;
    // Presence comes from the latch bits — M14 故障锁存 or M4 实时故障 — and
    // D110 only NAMES the fault (user decision 2026-09-24). The PLC program
    // never writes 0 back into D110 (every write in the ladder is MOV K1..K11),
    // so a code outlives the fault it describes; treating a non-zero code as
    // "still faulted" left 故障/急停 on screen after the operator had cleared
    // the latch with 复位. See activeAlarmText().
    const DeviceSnapshot &s = snapshot();
    return s.m14() || s.m4();
}

bool ShellModel::isEstop() const
{
    // M0 is in the fast block, M100 in the command readback block. Require
    // each source's own block quality so an untrusted (stale/errored) block
    // never fabricates an estop state (spec §8.2, §11.2; PLC-HMI-003 D6/D7
    // real block quality). A down link short-circuits first: the last snapshot
    // still claims connected()==true, so without this an estop that was
    // released after the outage began stayed on screen and kept the estop
    // control mapped to "release" (user decision 2026-09-24).
    if (!m_online)
        return false;
    const DeviceSnapshot &s = snapshot();
    const bool fastKnown =
        s.connected() && s.fastQuality() == DataQuality::Valid;
    const bool commandKnown =
        s.connected() && s.commandQuality() == DataQuality::Valid;
    return (fastKnown && s.m0()) || (commandKnown && s.m100());
}

QString ShellModel::userName() const
{
    return m_userName.isEmpty() ? QStringLiteral("未登录") : m_userName;
}

QString ShellModel::activeAlarmText() const
{
    // Priority (spec §11.1): offline > estop > latched fault > fault.
    //
    // Offline comes FIRST (user decision 2026-09-24). This used to read the
    // cached snapshot first and only then check the link, so after a link loss
    // it kept announcing the last snapshot's 急停/存在锁存故障 — states the
    // machine may have left minutes earlier. With no link there is no evidence
    // at all, and 通讯中断 is the only honest thing to show. The fault/estop
    // branches still read the bits fail-safe (no quality gating) once the link
    // is up: a possibly-stale latched fault is reported rather than hidden.
    if (!m_online)
        return QStringLiteral("通讯中断");
    if (m_snapshot.has_value()) {
        const DeviceSnapshot &s = *m_snapshot;
        if (s.m0() || s.m100())
            return QStringLiteral("急停有效");
        // The latch bits decide WHETHER there is a fault; D110 only says which
        // one (user decision 2026-09-24). Every D110 write in the PLC program is
        // MOV K1..K11 and nothing ever writes 0 back, so a stale code keeps
        // naming a fault that 复位 already cleared. While the latch is still on
        // the code is shown — it is the diagnosis the operator needs.
        if (s.m14() || s.m4()) {
            return s.faultCode() != 0 && !s.fault().meaning.isEmpty()
                ? s.fault().meaning
                : QStringLiteral("存在锁存故障");
        }
    }
    return QString();
}

} // namespace hlm
