#pragma once

// Passive serial-port discovery boundary (spec §8.1, ARCH-005, C-11).
//
// The operation is asynchronous: enumerateAvailablePorts() returns a non-zero
// enumeration_request_id immediately and the completion is announced through
// enumerationCompleted() carrying exactly {enumeration_request_id,
// discovered_descriptors, completion_error}. The boundary never opens, probes
// or sweeps a port, never identifies a PLC and never changes, closes or
// reconnects the active gateway.
//
// No QSerialPortInfo, QSerialPort/QModbus pointer, widget or SQL type may
// cross this port (spec NF-04).

#include <QObject>
#include <QString>
#include <QVector>

#include "domain/serial_port_descriptor.h"

namespace hlm {

struct SerialEnumerationResult {
    quint64 enumeration_request_id = 0;
    QVector<SerialPortDescriptor> discovered_descriptors;
    QString completion_error; // empty on success; a failed enumeration claims no ports
};

class ISerialPortDiscovery : public QObject
{
    Q_OBJECT

public:
    explicit ISerialPortDiscovery(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~ISerialPortDiscovery() override = default;

    // Starts one passive enumeration and returns its non-zero request id. The
    // matching completion is delivered through enumerationCompleted().
    virtual quint64 enumerateAvailablePorts() = 0;

signals:
    void enumerationCompleted(const SerialEnumerationResult &result);
};

} // namespace hlm

Q_DECLARE_METATYPE(hlm::SerialEnumerationResult)
