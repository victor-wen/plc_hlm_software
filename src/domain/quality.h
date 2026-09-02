#pragma once

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

} // namespace hlm
