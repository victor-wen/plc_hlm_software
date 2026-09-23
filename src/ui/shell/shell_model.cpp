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
    // M14 (D100 bit14) and D110 fault code both live in the fast block.
    const DeviceSnapshot &s = snapshot();
    return s.m14() || s.faultCode() != 0;
}

bool ShellModel::isEstop() const
{
    // M0 is in the fast block, M100 in the command readback block. Require
    // each source's own block quality so an untrusted (stale/errored) block
    // never fabricates an estop state (spec §8.2, §11.2; PLC-HMI-003 D6/D7
    // real block quality).
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
    // Priority (spec §11.1): estop > latched fault > fault > offline notice.
    // Deliberately reads the raw snapshot without block-quality gating:
    // fail-safe, a possibly-stale estop/fault is reported rather than hidden.
    if (m_snapshot.has_value()) {
        const DeviceSnapshot &s = *m_snapshot;
        if (s.m0() || s.m100())
            return QStringLiteral("急停有效");
        if (s.m14())
            return QStringLiteral("存在锁存故障");
        if (s.faultCode() != 0)
            return s.fault().meaning.isEmpty()
                ? QStringLiteral("故障 代码 %1").arg(s.faultCode())
                : s.fault().meaning;
    }
    if (!m_online)
        return QStringLiteral("通讯中断");
    return QString();
}

} // namespace hlm
