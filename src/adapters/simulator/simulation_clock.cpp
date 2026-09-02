#include "adapters/simulator/simulation_clock.h"

namespace hlm {

void SimulationClock::advance(quint64 seconds)
{
    m_elapsed += seconds;
}

} // namespace hlm
