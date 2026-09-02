#pragma once

#include <QString>

namespace hlm {

// User roles (spec §11.4).
enum class Role { Anonymous, Operator, Admin };

// Control operations gated by the permission matrix (spec §11.4).
enum class Command {
    Reset,          // 复位/回原点
    AdjustWidth,    // 配方应用调宽
    ModeSwitch,     // 模式切换 (M104)
    Start,          // 自动启动
    Stop,           // 在线停止
    EstopSet,       // 置软件急停 (M100=1)
    EstopRelease,   // 解除软件急停 (M100=0)
    ManualCommand,  // 手动命令 M106-M109
    Bypass,         // 直通/常转/屏蔽 M105/M42/M110/M111
    ParameterChange,// 用户/通讯/参数设置
    LogoutClear,    // 注销时清除 M42/M106-M111 (internal, not gated)
    Count
};

// Structured permission result: no magic booleans (spec §11.4).
struct PermissionResult {
    bool allowed = false;
    QString reason; // empty when allowed
};

// Pure role-based permission matrix (spec §11.4). No state, no I/O.
class PermissionPolicy
{
public:
    static PermissionResult check(Role role, Command cmd);
};

} // namespace hlm
