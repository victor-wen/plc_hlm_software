#pragma once

// Alarm edge detection (spec §12).
//
// PLC alarms: D110 going 0 -> non-zero, or changing to a different non-zero
// code, ends the old event and starts a new one. D110 returning to 0 with both
// M14 and M4 clear ends the current PLC alarm event. Unknown non-zero codes
// are preserved with a generic message (spec §12, §13).
//
// HMI alarms are driven by explicit start/end calls from the application
// (communication loss, heartbeat freeze, home timeout, database restricted
// mode, vision unavailable).
//
// The detector is a plain class used on the SQLite worker thread (spec §7.3).

#include <QDateTime>
#include <QString>

#include "ports/repositories.h"

namespace hlm {

class AlarmRepository;

class AlarmEdgeDetector
{
public:
    explicit AlarmEdgeDetector(AlarmRepository *repo) : m_repo(repo) {}

    // Feeds a PLC snapshot (D110, M14, M4, snapshot sequence). Persists edge
    // transitions per spec §12. Returns false on a repository failure.
    bool onPlcSnapshot(quint16 d110, bool m14, bool m4, quint64 sequence);

    // Starts an HMI system alarm. If one is already active for the HMI source
    // it is ended first (at most one active event per source).
    bool startHmiAlarm(const QString &message, AlarmSeverity severity,
                       quint64 sequence);

    // Ends the active HMI alarm, if any.
    bool endHmiAlarm(quint64 sequence);

private:
    AlarmRepository *m_repo;
    quint16 m_lastD110 = 0;
    bool m_lastM14 = false;
    bool m_lastM4 = false;
    bool m_initialized = false;
};

} // namespace hlm
