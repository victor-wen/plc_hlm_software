#pragma once

#include <QtGlobal>

namespace hlm {

// Data quality of a snapshot source block or field (spec §9).
enum class DataQuality {
    Valid = 0,        // fresh, in-range, protocol-consistent
    Stale = 1,        // data older than the freshness threshold
    ProtocolError = 2,// transport/CRC/response error
    OutOfRange = 3,   // decoded value outside the defined range
};

// Worst quality wins when aggregating (Valid < Stale < ProtocolError <
// OutOfRange). Used for the overall snapshot quality.
inline DataQuality worstQuality(DataQuality a, DataQuality b)
{
    return (static_cast<int>(a) >= static_cast<int>(b)) ? a : b;
}

// Approved stale thresholds (PLC-HMI-003 D6, contract invariant 461):
// a block without a successful transfer inside its threshold is Stale.
constexpr qint64 kFastStaleMs = 1000;
constexpr qint64 kHomeStaleMs = 1000;
constexpr qint64 kCommandStaleMs = 2000;
constexpr qint64 kSlowStaleMs = 4000;

// Evidence-based block quality: a transport failure stays ProtocolError until
// a successful refresh, otherwise the block is Stale once its last success is
// older than the approved threshold.
inline DataQuality ageAdjustedQuality(DataQuality transport, qint64 ageMs,
                                      qint64 staleThresholdMs)
{
    if (transport != DataQuality::Valid)
        return transport;
    return ageMs > staleThresholdMs ? DataQuality::Stale : DataQuality::Valid;
}

} // namespace hlm
