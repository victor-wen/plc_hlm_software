#pragma once

#include <QString>
#include <QVector>

namespace hlm {

// One entry of the PLC fault code table (D110). Codes 0-10 are defined by the
// requirement document; code 10 is "调宽定位超时" (spec §10.3). Any unknown
// non-zero code is treated as an unknown latched fault and must never crash.
struct FaultInfo {
    quint16 code = 0;
    QString meaning;
    bool isLatched = false; // latched faults require a reset (M14)
};

// Immutable fault code table; access via the singleton.
class FaultCodeTable
{
public:
    static const FaultCodeTable &instance();

    // Returns the info for `code` by value. Unknown non-zero codes map to an
    // "unknown latched fault" entry (code preserved, isLatched = true).
    // Returning by value keeps the table immutable and thread-safe: callers
    // never hold a reference that a later query could overwrite.
    FaultInfo info(quint16 code) const;

    const QVector<FaultInfo> &all() const { return m_entries; }

private:
    FaultCodeTable();
    QVector<FaultInfo> m_entries;
};

} // namespace hlm
