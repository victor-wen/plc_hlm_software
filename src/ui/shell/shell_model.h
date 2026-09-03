#pragma once

#include <QObject>
#include <QString>
#include <QHash>
#include <QDateTime>

#include "application/permission_policy.h"
#include "domain/device_snapshot.h"

namespace hlm {

// UI shell state source (spec §11.1-§11.2). Subscribes to the application
// layer: latest DeviceSnapshot, connection state, current user/role and
// command-pending flags. The shell widgets bind to this model. The UI never
// touches Modbus or SQL directly (spec Global Constraints).
//
// No optimistic state: mode/running/homed/fault are ONLY derived from the
// last confirmed snapshot; command submission only sets a "pending" flag
// shown as 发送中/等待确认 (spec §11.2).
class ShellModel : public QObject
{
    Q_OBJECT

public:
    explicit ShellModel(QObject *parent = nullptr);

    // --- snapshot feed (wired to gateway/coordinator by the app shell) ------
    void updateSnapshot(const DeviceSnapshot &s);
    void setOnline(bool online);

    // --- session -------------------------------------------------------------
    void setUser(const QString &name, Role role);
    void clearUser();

    // --- command pending flags (spec §11.2: 发送中/等待确认) ------------------
    void setCommandPending(Command cmd, bool pending);
    bool hasPendingCommands() const { return !m_pending.isEmpty(); }

    // --- derived state (snapshot-confirmed only) ------------------------------
    bool online() const { return m_online; }
    bool hasSnapshot() const { return m_snapshot.has_value(); }
    // Fresh = has snapshot, overall quality valid (not stale/error) and
    // connected. Drives "—" display and action disabling (spec §11.2).
    bool snapshotFresh() const;
    // Actions need a fresh snapshot + online link (spec §11.2: 过期/无效
    // 字段禁用依赖动作).
    bool actionsAvailable() const { return m_online && snapshotFresh(); }

    bool modeKnown() const { return snapshotFresh(); }
    bool isAutoMode() const;   // M2=1 (M1=0)
    bool isRunning() const;    // M3=1
    bool isHomed() const;      // M9 (M61 readback, spec §8.2)
    bool isFaulted() const;    // M14 or fault code != 0
    bool isEstop() const;      // M0 or M100

    QString userName() const;
    Role role() const { return m_role; }

    const DeviceSnapshot &snapshot() const
    {
        return m_snapshot ? *m_snapshot : m_emptySnapshot;
    }

    // Highest-priority active alarm text for the banner (spec §11.1):
    // estop > latched fault > fault > offline notice. Empty when none.
    QString activeAlarmText() const;

signals:
    // Emitted after any state change; shell widgets re-read the model.
    void stateChanged();
    void userChanged();

private:
    bool m_online = false;
    QString m_userName;
    Role m_role = Role::Anonymous;
    std::optional<DeviceSnapshot> m_snapshot;
    DeviceSnapshot m_emptySnapshot{DeviceSnapshotData()};
    QHash<int, bool> m_pending; // Command as int for QHash
};

} // namespace hlm