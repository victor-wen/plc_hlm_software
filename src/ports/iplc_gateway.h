#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>

#include "domain/device_snapshot.h"

namespace hlm {

// Command priority hint (spec §8.3). Maps onto the request queue's internal
// priority levels. PulseClear and Heartbeat are reserved for Task 5's pulse
// state machine and M112 watchdog; the gateway only needs to route them to
// the right queue level.
enum class CommandPriority {
    Normal = 0,     // user writes (queue level 4: other user writes)
    Safety = 1,     // online stop / estop set / continuous-motion clear (level 2)
    PulseClear = 2, // pulse clear requests (level 1) - used by Task 5
    Heartbeat = 3,  // M112 heartbeat flip (level 3) - used by Task 5
};

// Port interface for the PLC gateway (spec §7.2, §8). Implemented by the real
// Modbus gateway (Task 4) and later by the in-process SimulatedPlcGateway
// (Task 6). All methods are thread-safe: commands are marshalled onto the
// gateway's own worker thread; results and events arrive via signals.
class IPlcGateway : public QObject
{
    Q_OBJECT

public:
    explicit IPlcGateway(QObject *parent = nullptr) : QObject(parent) {}
    ~IPlcGateway() override = default;

    // Lifecycle. start() opens the link and begins polling; stop() closes it.
    virtual void start() = 0;
    virtual void stop() = 0;

    // True when the gateway has a live link and (after reconnect) has already
    // delivered at least one full valid snapshot.
    virtual bool isOnline() const = 0;

    // Asynchronous writes. Results arrive via writeCompleted().
    virtual void writeCoil(quint16 address, bool value,
                          CommandPriority priority = CommandPriority::Normal) = 0;
    virtual void writeRegister(quint16 address, quint16 value,
                               CommandPriority priority = CommandPriority::Normal) = 0;

    // Start a pulse on `address` (spec §8.5): serially write 1, hold at
    // least 100 ms, then clear. Returns false when the pulse could not be
    // started (offline, or a pulse on the same address is already active).
    // The pulse outcome is reported via writeCompleted(address, ok).
    virtual bool startPulse(quint16 address) = 0;

signals:
    // A complete, immutable device snapshot (spec §9).
    void snapshotReady(const DeviceSnapshot &snapshot);
    // Link state: true when online and a full snapshot has been delivered.
    void connectionStateChanged(bool online);
    // Result of a writeCoil/writeRegister call.
    void writeCompleted(quint16 address, bool ok, const QString &error);
};

} // namespace hlm

Q_DECLARE_METATYPE(hlm::CommandPriority)
