#pragma once

#include <QMetaType>
#include <QString>

#include <optional>

#include "domain/command.h"

namespace hlm {

// Lifecycle state of one operator command (contract interface
// OperatorCommandStatus, .ai/project-contract.yaml). Every user request
// produces an immediate visible Accepted/Pending or Rejected state and every
// accepted finite command converges to exactly one terminal state.
//
// The enum is declared in the nested namespace below and re-exported into hlm
// so the contract-visible name hlm::OperatorCommandState is unchanged. This
// also keeps the QString toString(OperatorCommandState) helper below visible
// to normal lookup while preventing argument-dependent lookup from making it
// a candidate in QtTest's unqualified QTest::toString(enum) call, which
// QCOMPARE needs (a same-namespace helper returning QString would otherwise
// break every QCOMPARE on this enum in black-box tests).
namespace command_state {
enum class OperatorCommandState {
    Idle,
    Rejected,
    Accepted,
    Pending,
    Succeeded,
    Failed,
    TimedOut,
    CommunicationsLost,
    GatewayReplaced
};
} // namespace command_state

using command_state::OperatorCommandState;

// True for states a command never transitions out of.
bool isTerminal(OperatorCommandState state);

// Stable, non-empty operator-facing identifier for each lifecycle state.
QString toString(OperatorCommandState state);

// One projected operator-command status shown by the persistent shell status
// surface. Carries only immutable values; no QWidget/QModbus/QSqlDatabase/
// transport-handle fields (contract forbidden_fields). request_id and
// gateway_generation stay empty/0 until PLC-HMI-003 wires the real
// request-identity/gateway-generation correlation.
struct OperatorCommandStatus {
    // Command::Count marks the idle/cleared status (no active command).
    Command command = Command::Count;
    OperatorCommandState lifecycle_state = OperatorCommandState::Idle;
    QString human_readable_detail;
    quint64 command_generation = 0;
    std::optional<quint64> request_id;
    quint64 gateway_generation = 0;
};

} // namespace hlm

Q_DECLARE_METATYPE(hlm::OperatorCommandStatus)
