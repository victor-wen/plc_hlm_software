#pragma once

#include <QtGlobal>

namespace hlm {

// Injectable, deterministic clock for the H3U simulation model (spec §14.1).
// Time only advances when advance() is called explicitly, so every scenario
// runs without real sleeps.
class SimulationClock
{
public:
    void advance(quint64 seconds);
    quint64 elapsed() const { return m_elapsed; }

private:
    quint64 m_elapsed = 0;
};

} // namespace hlm
