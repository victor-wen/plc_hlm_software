#pragma once

// Transport-neutral description of one locally available serial port
// (spec §8.1, ARCH-005). Produced by the passive enumeration adapter and
// presented to the administrator for explicit selection.
//
// The descriptor carries identity text plus optional USB vendor/product ids
// only: it never exposes an open handle, a probe result or a claimed PLC
// identity (spec C-11).

#include <QString>

#include <optional>

namespace hlm {

struct SerialPortDescriptor {
    QString port_name;    // e.g. "COM3" or "/dev/ttyUSB0"
    QString description;  // human-readable adapter description
    QString manufacturer; // e.g. "wch.cn"
    std::optional<quint16> optional_vendor_id;
    std::optional<quint16> optional_product_id;
};

} // namespace hlm
