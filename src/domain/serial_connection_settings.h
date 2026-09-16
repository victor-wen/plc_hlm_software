#pragma once

// Transport-neutral serial connection settings (spec §8.1, ARCH-005).
//
// This value is the canonical serial configuration shared by the application
// configuration, the Modbus gateway factory, the SQLite settings batch and the
// settings page. It carries no Qt serial enum, QSerialPortInfo object, widget,
// transport handle or SQL handle; the conversion to QSerialPort enums happens
// only inside hlm_modbus (spec NF-04, C-11).
//
// Defaults (spec §8.1): COM1, 站号 1, 波特率 9600, 停止位 1, 校验 无 (8N1),
// 超时 200 ms, 读重试 1.

#include <QString>

namespace hlm {

struct SerialConnectionSettings {
    QString port_name = QStringLiteral("COM1");
    int station = 1;     // 1-247
    int baud_rate = 9600; // 9600、19200
    int stop_bits = 1;   // 1、2
    QString parity = QStringLiteral("无"); // 无、奇、偶
    int timeout_ms = 200;
    int read_retries = 1;
};

} // namespace hlm
