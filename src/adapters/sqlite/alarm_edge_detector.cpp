#include "adapters/sqlite/alarm_edge_detector.h"

#include "domain/fault_code.h"
#include "ports/repositories.h"

namespace hlm {

namespace {

QString plcMessage(quint16 code)
{
    return FaultCodeTable::instance().info(code).meaning;
}

} // namespace

bool AlarmEdgeDetector::onPlcSnapshot(quint16 d110, bool m14, bool m4, quint64 sequence)
{
    if (!m_initialized) {
        // First snapshot: adopt the current state without persisting edges.
        m_lastD110 = d110;
        m_lastM14 = m14;
        m_lastM4 = m4;
        m_initialized = true;
        return true;
    }

    const bool codeChanged = d110 != m_lastD110;
    const bool cleared = d110 == 0 && !m14 && !m4;

    if (d110 != 0 && codeChanged) {
        // 0 -> non-zero, or a change between non-zero codes: end the previous
        // PLC event and start a new one (spec §12).
        if (const auto active = m_repo->activeAlarm(AlarmSource::Plc)) {
            if (!m_repo->endAlarm(active->id, sequence))
                return false;
        }
        AlarmEventRecord e;
        e.source = AlarmSource::Plc;
        e.code = d110;
        e.messageSnapshot = plcMessage(d110);
        e.severity = AlarmSeverity::Critical; // PLC faults are latched (spec §12)
        e.startedAt = QDateTime::currentDateTimeUtc();
        e.snapshotSequence = sequence;
        if (m_repo->startAlarm(e) < 0)
            return false;
    } else if (cleared) {
        // D110 is 0 and M14/M4 are both clear: end the active event, if any.
        // While M14 or M4 is set the event stays active (spec §12).
        if (const auto active = m_repo->activeAlarm(AlarmSource::Plc)) {
            if (!m_repo->endAlarm(active->id, sequence))
                return false;
        }
    }

    m_lastD110 = d110;
    m_lastM14 = m14;
    m_lastM4 = m4;
    return true;
}

bool AlarmEdgeDetector::startHmiAlarm(const QString &message, AlarmSeverity severity,
                                      quint64 sequence)
{
    if (const auto active = m_repo->activeAlarm(AlarmSource::Hmi)) {
        if (!m_repo->endAlarm(active->id, sequence))
            return false;
    }
    AlarmEventRecord e;
    e.source = AlarmSource::Hmi;
    e.code = 0;
    e.messageSnapshot = message;
    e.severity = severity;
    e.startedAt = QDateTime::currentDateTimeUtc();
    e.snapshotSequence = sequence;
    return m_repo->startAlarm(e) >= 0;
}

bool AlarmEdgeDetector::endHmiAlarm(quint64 sequence)
{
    if (const auto active = m_repo->activeAlarm(AlarmSource::Hmi))
        return m_repo->endAlarm(active->id, sequence);
    return true;
}

} // namespace hlm
