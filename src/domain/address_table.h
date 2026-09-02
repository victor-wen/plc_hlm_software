#pragma once

#include <QString>
#include <QVector>

namespace hlm {

// Access type of a PLC address (spec §8.2).
enum class AccessType { Read, ReadWrite };

// Numeric value type of a PLC address (spec §8.2).
enum class ValueType { U16, I16, U32, I32 };

// One entry of the centralized address table. UI and flow code must never
// use bare address numbers; look everything up through AddressTable.
struct AddressDef {
    QString area;      // "M" or "D"
    quint16 address;   // 0-based Modbus protocol address (M0 -> 0, D100 -> 100)
    QString name;      // e.g. "D100"
    QString description;
    AccessType access = AccessType::Read;
    ValueType valueType = ValueType::U16;
    quint16 highWord = 0; // for 32-bit values: address of the high word
    double min = 0.0;     // valid range (inclusive); 0/0 means unbounded
    double max = 0.0;
    double scale = 1.0;   // display scale factor
    QString unit;
};

// Central, single source of truth for every PLC address used by the HMI
// (spec §8.2). Immutable after construction; access via the singleton.
class AddressTable
{
public:
    static const AddressTable &instance();

    // Lookup by area + 0-based protocol address; nullptr if unknown.
    const AddressDef *find(const QString &area, quint16 address) const;
    // Lookup by display name (e.g. "D140"); nullptr if unknown.
    const AddressDef *findByName(const QString &name) const;

    const QVector<AddressDef> &all() const { return m_defs; }

private:
    AddressTable();
    QVector<AddressDef> m_defs;
};

} // namespace hlm
