#include "adapters/modbus/qt_serial_port_discovery.h"

#include <QMetaObject>
#include <QSerialPortInfo>
#include <QThread>
#include <QVector>

namespace hlm {

namespace {

// Passive mapping of the OS port list onto transport-neutral descriptors. This
// is the only place QSerialPortInfo appears; no port is opened and no PLC
// property is queried.
QVector<SerialPortDescriptor> passivelyListPorts()
{
    QVector<SerialPortDescriptor> descriptors;
    const QList<QSerialPortInfo> ports = QSerialPortInfo::availablePorts();
    descriptors.reserve(ports.size());
    for (const QSerialPortInfo &info : ports) {
        SerialPortDescriptor descriptor;
        descriptor.port_name = info.portName();
        descriptor.description = info.description();
        descriptor.manufacturer = info.manufacturer();
        if (info.hasVendorIdentifier())
            descriptor.optional_vendor_id = info.vendorIdentifier();
        if (info.hasProductIdentifier())
            descriptor.optional_product_id = info.productIdentifier();
        descriptors.append(descriptor);
    }
    return descriptors;
}

} // namespace

QtSerialPortDiscovery::QtSerialPortDiscovery(QObject *parent)
    : ISerialPortDiscovery(parent)
{
    qRegisterMetaType<SerialEnumerationResult>("hlm::SerialEnumerationResult");

    // The OS enumeration runs on a private worker so a slow port scan cannot
    // block the UI thread; results are queued back to this object's thread.
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("serial-port-discovery"));
    m_worker = new QObject;
    m_worker->moveToThread(m_thread);
    m_thread->start();
}

QtSerialPortDiscovery::~QtSerialPortDiscovery()
{
    if (!m_thread)
        return;
    m_thread->quit();
    m_thread->wait();
    delete m_worker;
    m_worker = nullptr;
}

quint64 QtSerialPortDiscovery::enumerateAvailablePorts()
{
    const quint64 requestId = m_nextRequestId++;
    if (!m_worker)
        return requestId;

    QMetaObject::invokeMethod(
        m_worker,
        [this, requestId]() {
            SerialEnumerationResult result;
            result.enumeration_request_id = requestId;
            result.discovered_descriptors = passivelyListPorts();
            // Emitted from the worker thread; the connection to any receiver
            // is queued, so the completion crosses back as an immutable value.
            emit enumerationCompleted(result);
        },
        Qt::QueuedConnection);
    return requestId;
}

} // namespace hlm
