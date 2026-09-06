#pragma once

// Application configuration (spec §8.1, §11.5, §12). Plain value struct
// assembled by the composition root (src/app/application.cpp) from defaults,
// persisted settings and command-line overrides. No I/O here.

#include <QString>

#include "ui/pages/users_settings_model.h" // SerialConfig

namespace hlm {

struct AppConfig {
    // --- database (spec §12) -------------------------------------------------
    // Default machine-level data directory is %ProgramData%\PLC-HLM\.
    QString databasePath = QStringLiteral("PLC-HLM/app.db");

    // --- serial (spec §8.1) ---------------------------------------------------
    SerialConfig serial; // defaults: COM1, station 1, 9600 8N1

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
};

} // namespace hlm
