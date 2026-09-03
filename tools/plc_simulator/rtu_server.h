#pragma once

#include <QModbusExceptionResponse>
#include <QModbusPdu>
#include <QModbusRequest>
#include <QModbusResponse>
#include <QObject>
#include <QString>
#include <QVector>

#include "adapters/simulator/h3u_simulation_model.h"
#include "tools/plc_simulator/fault_injection.h"

namespace hlm {

// One logged request, shown by the control panel (Task 19).
struct RtuRequestLogEntry {
    quint8 functionCode = 0;
    quint16 address = 0;
    quint16 count = 0;   // read count (coils/registers)
    quint16 value = 0;   // write value
    bool responded = false; // false = timeout injection (no response sent)
    quint8 exceptionCode = 0; // 0 = normal response
};

// Unit-testable core of the RTU simulator (spec §14.3). Handles the Modbus
// PDU layer: function codes 01/03/05/06 against the shared H3uSimulationModel
// (0-based protocol addresses, matching the production gateway) plus the
// PDU-level fault injections. No Qt serial wiring here, so the whole backend
// logic is testable without a virtual COM pair.
//
// The model clock is advanced by tick() (1 simulated second per call), which
// drives D140, positioning, home return and the M112 watchdog exactly like
// the in-process SimulatedPlcGateway.
class RtuRequestHandler
{
public:
    RtuRequestHandler(H3uSimulationModel &model, FaultInjector &faults);

    // Process one request PDU. Returns an invalid response for the Timeout
    // scenario (the RTU layer then sends nothing) and an exception response
    // for the ExceptionResponse scenario.
    QModbusResponse handleRequest(const QModbusRequest &request);

    // Advance the simulated PLC time by one second.
    void tick();

    // Request log for the control panel.
    const QVector<RtuRequestLogEntry> &requestLog() const { return m_log; }
    void clearRequestLog() { m_log.clear(); }

private:
    QModbusResponse handleReadCoils(const QModbusRequest &req);
    QModbusResponse handleReadHoldingRegisters(const QModbusRequest &req);
    QModbusResponse handleWriteSingleCoil(const QModbusRequest &req);
    QModbusResponse handleWriteSingleRegister(const QModbusRequest &req);

    void logRequest(const QModbusRequest &req, const QModbusResponse &resp);

    H3uSimulationModel &m_model;
    FaultInjector &m_faults;
    QVector<RtuRequestLogEntry> m_log;
};

// Qt wiring for the standalone simulator (spec §14.3): a QModbusRtuSerialServer
// over a serial port (a Windows virtual COM pair in production) backed by the
// shared model. The station must be an explicit 1-247 value: 0/255 would
// answer broadcast frames and mask addressing errors, so start() rejects them.
class RtuServer : public QObject
{
    Q_OBJECT

public:
    RtuServer(H3uSimulationModel &model, FaultInjector &faults,
              QObject *parent = nullptr);
    ~RtuServer() override;

    // Open the serial port and start serving. Returns false (and leaves the
    // server stopped) when the station is not in 1-247 or the port cannot be
    // opened.
    bool start(const QString &portName, int baudRate, int station);
    void stop();

    bool isRunning() const { return m_running; }
    int station() const { return m_station; }

    // Exposed for the control panel (Task 19).
    RtuRequestHandler &handler() { return m_handler; }
    const RtuRequestHandler &handler() const { return m_handler; }

private:
    class Server;
    Server *m_server = nullptr;
    RtuRequestHandler m_handler;
    FaultInjector &m_faults;
    bool m_running = false;
    int m_station = 0;
};

} // namespace hlm
