#include "domain/operator_command_status.h"

namespace hlm {

bool isTerminal(OperatorCommandState state)
{
    switch (state) {
    case OperatorCommandState::Succeeded:
    case OperatorCommandState::Failed:
    case OperatorCommandState::TimedOut:
    case OperatorCommandState::CommunicationsLost:
    case OperatorCommandState::GatewayReplaced:
        return true;
    case OperatorCommandState::Idle:
    case OperatorCommandState::Rejected:
    case OperatorCommandState::Accepted:
    case OperatorCommandState::Pending:
        return false;
    }
    return false;
}

QString toString(OperatorCommandState state)
{
    switch (state) {
    case OperatorCommandState::Idle:
        return QStringLiteral("空闲");
    case OperatorCommandState::Rejected:
        return QStringLiteral("已拒绝");
    case OperatorCommandState::Accepted:
        return QStringLiteral("已接受");
    case OperatorCommandState::Pending:
        return QStringLiteral("等待确认");
    case OperatorCommandState::Succeeded:
        return QStringLiteral("成功");
    case OperatorCommandState::Failed:
        return QStringLiteral("失败");
    case OperatorCommandState::TimedOut:
        return QStringLiteral("超时");
    case OperatorCommandState::CommunicationsLost:
        return QStringLiteral("通讯中断");
    case OperatorCommandState::GatewayReplaced:
        return QStringLiteral("网关已更换");
    }
    return QStringLiteral("未知状态");
}

} // namespace hlm
