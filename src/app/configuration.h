#pragma once

// Application configuration (spec §8.1, §11.5, §12). Plain value struct
// assembled by the composition root (src/app/application.cpp) from defaults,
// persisted settings and command-line overrides. No I/O here.
//
// The serial settings are the transport-neutral domain value and the passive
// discovery boundary is port-typed: this layer must not depend on UI-owned or
// concrete serial types (D1, ARCH-005).

#include <QString>

#include "domain/serial_connection_settings.h"

namespace hlm {

class ISerialPortDiscovery;
class IPlcGateway;

struct AppConfig {
    // --- database (spec §12) -------------------------------------------------
    // Default machine-level data directory is %ProgramData%\PLC-HLM\.
    QString databasePath = QStringLiteral("PLC-HLM/app.db");

    // --- serial (spec §8.1) ---------------------------------------------------
    SerialConnectionSettings serial; // defaults: COM1, station 1, 9600 8N1

    // Injected passive serial-port discovery boundary. When null the
    // composition root creates the real QtSerialPortDiscovery adapter; the
    // configuration itself carries no concrete serial or UI type.
    ISerialPortDiscovery *serialPortDiscovery = nullptr;

    // --- session (spec §11.5) -------------------------------------------------
    int sessionTimeoutSec = 900; // 15 分钟无操作自动注销
    int sessionWarningSec = 60;  // 提前 60 秒提示

    // --- retention (spec §12) -------------------------------------------------
    int retentionDays = 365;

    // --- control (spec §10.2) -------------------------------------------------
    int resetTimeoutSec = 120; // HMI 防御性复位超时 30-600

    // --- gateway selection -----------------------------------------------------
    // True: in-process SimulatedPlcGateway (only when explicitly requested
    // with --sim). False: real QtModbusPlcGateway over the serial port.
    // Production is deliberately the default: absence of a physical/virtual
    // PLC connection must never be presented as "online".
    bool useSimulatedGateway = false;

    // The application advances the in-process simulator in real time so
    // --sim is usable interactively. Set to 0 in deterministic tests that
    // advance SimulatedPlcGateway::tick() explicitly.
    int simulatedTickIntervalMs = 1000;

    // --- injected gateway (S-INJECT, PLC-HMI-007 D7) --------------------------
    // Optional caller-owned gateway. When null the composition root composes
    // the gateway selected by useSimulatedGateway above; when non-null that
    // instance is used as the initial gateway and receives the assigned
    // generation like any other. The configuration never owns it: Application
    // neither deletes nor reparents it, and the caller keeps it alive for the
    // whole application lifetime (same pattern as serialPortDiscovery).
    IPlcGateway *plcGateway = nullptr;
};

} // namespace hlm
