#pragma once

#include <QString>
#include <QVector>

#include "domain/device_snapshot.h"

namespace hlm {

class ShellModel;

// One rendered field: display text plus validity. `valid` false renders as
// "—" (spec §9: 字段过期或无效时显示"—").
struct DiagnosticsField
{
    QString text;
    bool valid = false;
};

// One named bit row: M number, name, state (true/false/unknown).
struct BitRow
{
    int mNumber = 0;
    QString name;
    bool state = false;
    bool known = false; // false -> "—" (stale snapshot)
};

// Communication statistics fed by the app shell (Task 20 wires the gateway
// counters here). The page only displays them; it never computes them.
struct CommStats
{
    qint64 lastDataAgeMs = 0;
    quint64 sequence = 0;
    int reconnectCount = 0;
    int failedPolls = 0;
};

// Maps the latest ShellModel snapshot to the I/O 与诊断 page display
// (spec §8.2, §9, §11.3). Pure read-only mapping over ShellModel plus two
// externally-fed diagnostics inputs (vision self-test result and comm
// statistics, wired by Task 20). No I/O, no commands, no write intents.
//
// Bit exposure is strictly limited to the DEFINED bits (spec §8.2):
//   - D100 bit0-14 -> M0-M14 (bit15 reserved, never exposed)
//   - D103 bit0-15 -> M30-M45
//   - M50-M53 (homeBits) and M100-M112 (commandBits)
// D102/D104/D105 have no defined bits (acceptance): they are exposed ONLY as
// raw hex words, never parsed into bits.
//
// D140 activity (spec §13): the model tracks the last snapshot's heartbeat
// and sequence; a heartbeat change between consecutive snapshots = active,
// unchanged = frozen. Non-snapshot state changes (user/online) never flip it.
class DiagnosticsModel
{
public:
    explicit DiagnosticsModel(const ShellModel &model);

    // --- raw status words D100-D105 (spec §8.2) ------------------------------
    // index 0=D100, 1=D102, 2=D103, 3=D104, 4=D105. D102/D104/D105 are raw
    // only (acceptance). Returns "—" when the snapshot is not fresh.
    bool rawWordsValid() const;
    QString rawWordHex(int index) const;

    // --- defined bits (spec §8.2) --------------------------------------------
    // True when the snapshot is fresh enough to trust bit states.
    bool bitValid() const;
    // State of a DEFINED bit: M0-M14 (D100), M30-M45 (D103), M50-M53,
    // M100-M112. Returns false for any undefined M number (e.g. D102/D104/
    // D105 bits M200/M300/M316 are never exposed).
    bool bitState(int mNumber) const;
    // All defined bit rows for the page tables.
    QVector<BitRow> d100Bits() const;   // M0-M14
    QVector<BitRow> d103Bits() const;  // M30-M35, M40-M45
    QVector<BitRow> homeCommandBits() const; // M50-M53, M100-M112

    // --- D140 heartbeat activity (spec §13) ----------------------------------
    bool heartbeatKnown() const;
    // Tracks the last snapshot's heartbeat across calls (mutable: activity is
    // judged between consecutive snapshots, so reading it advances the
    // comparison state).
    bool heartbeatActive() const;

    // --- key registers (spec §11.3) -------------------------------------------
    DiagnosticsField faultCode() const;      // D110
    DiagnosticsField currentStep() const;    // D120
    DiagnosticsField beltSpeed() const;      // D122
    DiagnosticsField widthFrequency() const;  // D126/127
    DiagnosticsField targetWidth() const;    // D128
    DiagnosticsField currentWidth() const;   // D130
    DiagnosticsField pulseCount() const;     // D136/137
    DiagnosticsField productionCount() const;// D138/139
    DiagnosticsField pulsePerMm() const;     // D204
    DiagnosticsField widthDelta() const;    // D210
    DiagnosticsField widthSpeed() const;     // D220

    // --- vision self-test (wired by Task 20, spec §7.4/§13) ------------------
    void setVisionStatus(const QString &version, bool healthy,
                         const QString &failureReason);
    QString visionVersion() const { return m_visionVersion; }
    bool visionHealthy() const { return m_visionHealthy; }
    QString visionFailureReason() const { return m_visionFailureReason; }

    // --- comm statistics (wired by Task 20) -----------------------------------
    void setCommStats(const CommStats &stats) { m_commStats = stats; }
    const CommStats &commStats() const { return m_commStats; }

private:
    bool fresh() const;
    DiagnosticsField field(const QString &text, quint8 f) const;

    const ShellModel &m_model;

    // Last snapshot's heartbeat + sequence, for D140 activity tracking.
    mutable bool m_hasLastHeartbeat = false;
    mutable quint16 m_lastHeartbeat = 0;
    mutable quint64 m_lastSequence = 0;
    mutable bool m_lastActive = false;

    QString m_visionVersion;
    bool m_visionHealthy = false;
    QString m_visionFailureReason;
    CommStats m_commStats;
};

} // namespace hlm
