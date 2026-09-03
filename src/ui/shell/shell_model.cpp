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

bool ShellModel::isAutoMode() const
{
    if (!snapshotFresh())
        return false;
    return snapshot().m2();
}

bool ShellModel::isRunning() const
{
    if (!snapshotFresh())
        return false;
    return snapshot().m3();
}

bool ShellModel::isHomed() const
{
    if (!snapshotFresh())
        return false;
    return snapshot().m9(); // M61 via D100 bit9 (spec §8.2)
}

bool ShellModel::isFaulted() const
{
    if (!snapshotFresh())
        return false;
    const DeviceSnapshot &s = snapshot();
    return s.m14() || s.faultCode() != 0;
}

bool ShellModel::isEstop() const
{
    if (!snapshotFresh())
        return false;
    const DeviceSnapshot &s = snapshot();
    return s.m0() || s.m100();
}

QString ShellModel::userName() const
{
    return m_userName.isEmpty() ? QStringLiteral("未登录") : m_userName;
}

QString ShellModel::activeAlarmText() const
{
    if (!m_online)
        return QStringLiteral("通讯中断");
    if (!m_snapshot.has_value())
        return QString();
    const DeviceSnapshot &s = *m_snapshot;
    if (s.m0() || s.m100())
        return QStringLiteral("急停有效");
    if (s.faultCode() != 0)
        return s.fault().meaning.isEmpty()
            ? QStringLiteral("故障 代码 %1").arg(s.faultCode())
            : s.fault().meaning;
    if (s.m14())
        return QStringLiteral("存在锁存故障");
    return QString();
}

} // namespace hlm