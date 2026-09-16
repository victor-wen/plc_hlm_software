#pragma once

// Passive serial-port enumeration adapter (spec §8.1, ARCH-005, C-11).
//
// Implements the transport-neutral ISerialPortDiscovery boundary with
// QSerialPortInfo::availablePorts() only. The adapter never opens a port,
// never probes or sweeps serial parameters, never identifies a PLC and never
// touches the active gateway: enumeration is a passive listing of what the OS
// currently reports (spec C-11).
//
// The blocking OS call runs on a private worker thread and the completion is
// delivered as an immutable SerialEnumerationResult, so calling
// enumerateAvailablePorts() from the UI thread never blocks it (spec NF-01).

#include <QObject>

#include "ports/iserial_port_discovery.h"

class QThread;

namespace hlm {

class QtSerialPortDiscovery : public ISerialPortDiscovery
{
    Q_OBJECT

public:
    explicit QtSerialPortDiscovery(QObject *parent = nullptr);
    ~QtSerialPortDiscovery() override;

    // Starts one passive enumeration and returns its non-zero request id. The
    // completion arrives asynchronously through enumerationCompleted().
    quint64 enumerateAvailablePorts() override;

private:
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    quint64 m_nextRequestId = 1;
};

} // namespace hlm
