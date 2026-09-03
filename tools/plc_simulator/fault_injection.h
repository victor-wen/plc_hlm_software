#pragma once

#include <QtGlobal>

namespace hlm {

// Fault-injection state for the standalone RTU simulator (spec §14.3).
// The control panel (Task 19) sets the scenario; the backend only exposes
// the interface. The scenario is a plain value object so it can be driven
// directly in unit tests without a UI.
class FaultInjector
{
public:
    enum class Scenario {
        None,             // normal operation
        Timeout,          // do not respond to requests
        ExceptionResponse, // answer every request with a Modbus exception
        IllegalValue,     // reject writes with IllegalDataValue
        Disconnect,       // drop the serial link on the next request
        Delay,            // delay responses by delayMs
        HeartbeatFreeze,  // D140 stops changing (dead PLC)
        FaultCode,        // inject a home-return fault code into the model
    };

    Scenario scenario() const { return m_scenario; }
    void setScenario(Scenario s) { m_scenario = s; }

    // ExceptionResponse: the exception code to return (Modbus exception code).
    quint8 exceptionCode() const { return m_exceptionCode; }
    void setExceptionCode(quint8 code) { m_exceptionCode = code; }

    // Delay: response delay in milliseconds.
    int delayMs() const { return m_delayMs; }
    void setDelayMs(int ms) { m_delayMs = ms; }

    // FaultCode: home-return fault code injected into the model (8/9).
    int faultCode() const { return m_faultCode; }
    void setFaultCode(int code) { m_faultCode = code; }

private:
    Scenario m_scenario = Scenario::None;
    quint8 m_exceptionCode = 0x01; // IllegalFunction
    int m_delayMs = 0;
    int m_faultCode = 0;
};

} // namespace hlm
