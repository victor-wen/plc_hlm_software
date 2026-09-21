#pragma once

#include <QString>
#include <QtGlobal>

#include <cmath>

namespace hlm {

// D128 (target width), D130 (current width) and D210 (target - current) are
// transmitted in 0.1 mm units (user decision 2026-09-21): raw 2000 == 200.0 mm.
//
// Raw register values are the ONLY form written to the PLC, stored in the
// snapshot, compared for equality and persisted in the recipe row. The decimal
// form exists for display text and for operator entry, and nothing else. Keep
// the conversion at the two boundaries named below so a 10x unit mistake has
// nowhere to hide:
//   raw -> text      : width_units::rawToDisplay / rawDeltaToDisplay
//   operator -> raw  : width_units::displayMmToRaw (spin box) / parse of the
//                      same decimal form at the recipe boundary.
namespace width_units {

// One register unit is one tenth of a millimetre.
constexpr int kRawPerMm = 10;

// Neutral editor value used when there is no selection. Matches the PLC's
// first-scan D128 initialisation (MAIN.LD: MOV K100 D128).
constexpr quint16 kNeutralTargetRaw = 100;

// Raw register value -> display text with exactly one decimal place.
// 2000 -> "200.0", 100 -> "10.0".
inline QString rawToDisplay(quint16 raw)
{
    return QString::number(raw / 10.0, 'f', 1);
}

// Signed raw delta (D210) -> display text. -350 -> "-35.0".
inline QString rawDeltaToDisplay(qint16 raw)
{
    return QString::number(raw / 10.0, 'f', 1);
}

// Operator-entered millimetres -> raw register value, rounded to 0.1 mm.
// Values outside the u16 register range are clamped; the caller owns the
// operator-facing range gate (see InterlockRules::checkAdjustWidth).
inline quint16 mmToRaw(double mm)
{
    const double raw = mm * kRawPerMm;
    if (!(raw > 0.0))
        return 0;
    if (raw >= 65535.0)
        return 65535;
    return static_cast<quint16>(std::lround(raw));
}

} // namespace width_units

} // namespace hlm
