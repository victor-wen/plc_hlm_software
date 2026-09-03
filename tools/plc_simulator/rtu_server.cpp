#include "tools/plc_simulator/rtu_server.h"

#include <QBitArray>
#include <QModbusDataUnit>
#include <QModbusDevice>
#include <QModbusRtuSerialServer>
#include <QThread>

namespace hlm {

namespace {

// Shared model address spaces (spec §14.1): coils M0-M112, registers D100-D223.
constexpr int kCoilCount = 113;
constexpr int kRegisterCount = 224;

// Modbus protocol limits (spec §15.5 uses single-register/coil writes and
// block reads well within these).
constexpr int kMaxReadCoils = 2000;
constexpr int kMaxReadRegisters = 125;

} // namespace

// --- RtuRequestHandler -------------------------------------------------------

RtuRequestHandler::RtuRequestHandler(H3uSimulationModel &model, FaultInjector &faults)
    : m_model(model)
    , m_faults(faults)
{
}

QModbusResponse RtuRequestHandler::handleRequest(const QModbusRequest &request)
{
    // Timeout injection: consume the request, send nothing.
    if (m_faults.scenario() == FaultInjector::Scenario::Timeout) {
        logRequest(request, QModbusResponse());
        return QModbusResponse();
    }

    // Exception-response injection: answer with the configured exception code.
    if (m_faults.scenario() == FaultInjector::Scenario::ExceptionResponse) {
        const QModbusResponse resp = QModbusExceptionResponse(
            request.functionCode(),
            QModbusExceptionResponse::ExceptionCode(m_faults.exceptionCode()));
        logRequest(request, resp);
        return resp;
    }

    // Delay injection: hold the response for the configured duration.
    if (m_faults.scenario() == FaultInjector::Scenario::Delay && m_faults.delayMs() > 0)
        QThread::msleep(quint32(m_faults.delayMs()));

    QModbusResponse resp;
    switch (request.functionCode()) {
    case QModbusRequest::ReadCoils:
        resp = handleReadCoils(request);
        break;
    case QModbusRequest::ReadHoldingRegisters:
        resp = handleReadHoldingRegisters(request);
        break;
    case QModbusRequest::WriteSingleCoil:
        resp = handleWriteSingleCoil(request);
        break;
    case QModbusRequest::WriteSingleRegister:
        resp = handleWriteSingleRegister(request);
        break;
    default:
        resp = QModbusExceptionResponse(request.functionCode(),
                                        QModbusExceptionResponse::IllegalFunction);
        break;
    }

    logRequest(request, resp);
    return resp;
}

void RtuRequestHandler::tick()
{
    // Heartbeat-freeze injection: the model clock does not advance, so D140
    // stays frozen (dead PLC, spec §8.4).
    if (m_faults.scenario() == FaultInjector::Scenario::HeartbeatFreeze)
        return;
    // Fault-code injection: arm the model's home-return fault hook so the
    // fault surfaces on the next home return (spec §14.1).
    if (m_faults.scenario() == FaultInjector::Scenario::FaultCode)
        m_model.setHomeReturnFault(m_faults.faultCode());
    m_model.advance(1);
}

QModbusResponse RtuRequestHandler::handleReadCoils(const QModbusRequest &req)
{
    quint16 address = 0;
    quint16 count = 0;
    req.decodeData(&address, &count);

    if (count < 1 || count > kMaxReadCoils)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataValue);
    if (quint32(address) + count > kCoilCount)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataAddress);

    // Pack one bit per coil, LSB first (Modbus bit order).
    QBitArray bits(count);
    for (int i = 0; i < count; ++i)
        bits.setBit(i, m_model.readCoil(quint16(address + i)));

    const int byteCount = (count + 7) / 8;
    QByteArray payload = QByteArray::fromRawData(bits.bits(), byteCount);
    payload.prepend(char(byteCount));
    return QModbusResponse(req.functionCode(), payload);
}

QModbusResponse RtuRequestHandler::handleReadHoldingRegisters(const QModbusRequest &req)
{
    quint16 address = 0;
    quint16 count = 0;
    req.decodeData(&address, &count);

    if (count < 1 || count > kMaxReadRegisters)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataValue);
    if (quint32(address) + count > kRegisterCount)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataAddress);

    QList<quint16> values;
    values.reserve(count);
    for (int i = 0; i < count; ++i)
        values.append(m_model.readRegister(quint16(address + i)));
    return QModbusResponse(req.functionCode(), quint8(count * 2), values);
}

QModbusResponse RtuRequestHandler::handleWriteSingleCoil(const QModbusRequest &req)
{
    quint16 address = 0;
    quint16 value = 0;
    req.decodeData(&address, &value);

    if (value != 0x0000 && value != 0xFF00)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataValue);
    if (address >= kCoilCount)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataAddress);

    // Illegal-value injection: reject the write, leave the model untouched.
    if (m_faults.scenario() == FaultInjector::Scenario::IllegalValue)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataValue);

    // Conditional-failure injection (spec §14.3 条件失败): a width-adjust
    // command is accepted (echo response) but always fails its preconditions.
    // The command is consumed without reaching the model, so positioning can
    // never start regardless of the current model state.
    if (m_faults.scenario() == FaultInjector::Scenario::ConditionalFailure
        && address == 43) {
        m_model.writeCoil(45, true);
        m_model.writeCoil(34, false);
        m_model.writeCoil(44, false);
        return QModbusResponse(req.functionCode(), address, value);
    }

    m_model.writeCoil(address, value == 0xFF00);
    return QModbusResponse(req.functionCode(), address, value);
}

QModbusResponse RtuRequestHandler::handleWriteSingleRegister(const QModbusRequest &req)
{
    quint16 address = 0;
    quint16 value = 0;
    req.decodeData(&address, &value);

    if (address >= kRegisterCount)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataAddress);

    // Illegal-value injection: reject the write, leave the model untouched.
    if (m_faults.scenario() == FaultInjector::Scenario::IllegalValue)
        return QModbusExceptionResponse(req.functionCode(),
                                         QModbusExceptionResponse::IllegalDataValue);

    m_model.writeRegister(address, value);
    return QModbusResponse(req.functionCode(), address, value);
}

void RtuRequestHandler::logRequest(const QModbusRequest &req, const QModbusResponse &resp)
{
    RtuRequestLogEntry entry;
    entry.functionCode = quint8(req.functionCode());
    req.decodeData(&entry.address, &entry.count);
    if (req.functionCode() == QModbusRequest::WriteSingleCoil
        || req.functionCode() == QModbusRequest::WriteSingleRegister) {
        entry.value = entry.count;
        entry.count = 0;
    }
    entry.responded = resp.isValid();
    if (resp.isException())
        entry.exceptionCode = quint8(resp.exceptionCode());
    m_log.append(entry);
}

// --- RtuServer ---------------------------------------------------------------

// QModbusRtuSerialServer subclass: the serial layer answers the configured
// station only (Qt filters other stations at the ADU level) and forwards
// every request PDU to the shared handler. The Disconnect scenario drops the
// link on the next request.
class RtuServer::Server : public QModbusRtuSerialServer
{
    Q_OBJECT

public:
    Server(RtuRequestHandler &handler, FaultInjector &faults)
        : m_handler(handler)
        , m_faults(faults)
    {
    }

    bool doOpen() { return open(); }
    void doClose() { close(); }

signals:
    void linkDropped();

protected:
    QModbusResponse processRequest(const QModbusPdu &request) override
    {
        if (m_faults.scenario() == FaultInjector::Scenario::Disconnect) {
            // Consume the request, then drop the link (spec §14.3 断线).
            close();
            emit linkDropped();
            return QModbusResponse();
        }
        return m_handler.handleRequest(QModbusRequest(request));
    }

private:
    RtuRequestHandler &m_handler;
    FaultInjector &m_faults;
};

RtuServer::RtuServer(H3uSimulationModel &model, FaultInjector &faults, QObject *parent)
    : QObject(parent)
    , m_handler(model, faults)
    , m_faults(faults)
{
}

RtuServer::~RtuServer()
{
    stop();
}

bool RtuServer::start(const QString &portName, int baudRate, int station)
{
    // Spec §14.3: an explicit station in 1-247. 0/255 would answer broadcast
    // frames and mask addressing errors.
    if (station < 1 || station > 247)
        return false;

    stop();

    auto *server = new Server(m_handler, m_faults);
    connect(server, &Server::linkDropped, this, [this]() { m_running = false; });
    server->setServerAddress(station);
    server->setConnectionParameter(QModbusDevice::SerialPortNameParameter, portName);
    server->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, baudRate);
    server->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, 8);
    server->setConnectionParameter(QModbusDevice::SerialParityParameter, 0);
    server->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, 1);

    if (!server->doOpen()) {
        delete server;
        return false;
    }

    m_server = server;
    m_station = station;
    m_running = true;
    return true;
}

void RtuServer::stop()
{
    if (m_server) {
        m_server->doClose();
        delete m_server;
        m_server = nullptr;
    }
    m_running = false;
    m_station = 0;
}

} // namespace hlm

#include "rtu_server.moc"
