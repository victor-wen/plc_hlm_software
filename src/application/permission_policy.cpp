#include "application/permission_policy.h"

namespace hlm {

PermissionResult PermissionPolicy::check(Role role, Command cmd)
{
    PermissionResult r;
    switch (cmd) {
    case Command::Stop:
    case Command::EstopSet:
        // 在线停止、置软件急停: 未登录/操作员/管理员均可 (spec §10.5, §10.6).
        r.allowed = true;
        return r;
    case Command::Start:
        // 启动: 操作员和管理员 (spec §10.4).
        r.allowed = (role == Role::Operator || role == Role::Admin);
        if (!r.allowed)
            r.reason = QStringLiteral("需要操作员或管理员权限");
        return r;
    case Command::Reset:
    case Command::AdjustWidth:
    case Command::ModeSwitch:
    case Command::EstopRelease:
    case Command::ManualCommand:
    case Command::Bypass:
    case Command::ParameterChange:
        // 模式切换、复位、解除急停、配方调宽、手动、屏蔽、设置: 仅管理员
        // (spec §11.4).
        r.allowed = (role == Role::Admin);
        if (!r.allowed)
            r.reason = QStringLiteral("需要管理员权限");
        return r;
    case Command::LogoutClear:
        // 注销清 M42/M106-M111 是内部流程, 不受权限门控 (spec §11.5).
        r.allowed = true;
        return r;
    case Command::Count:
        break;
    }
    r.allowed = false;
    r.reason = QStringLiteral("未知命令");
    return r;
}

} // namespace hlm
