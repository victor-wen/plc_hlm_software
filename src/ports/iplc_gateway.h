#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>

#include "domain/device_snapshot.h"

namespace hlm {

// Command priority hint (spec §8.3). Maps onto the request queue's internal
// priority levels. PulseClear is reserved for Task 5's pulse state machine;
// the gateway only needs to route it to the top of the queue.
enum class CommandPriority {
    Normal = 0,     // user writes (queue level 4: other user writes)
    Safety = 1,     // online stop / estop set / continuous-motion clear (level 2)
    PulseClear = 2, // pulse clear requests (level 1) - used by Task 5
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
