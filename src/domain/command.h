#pragma once

namespace hlm {

// Control operations gated by the permission matrix (spec §11.4). This enum
// lives in the domain layer so domain values (OperatorCommandStatus) can carry
// a command identity without a domain -> application dependency. The
// application permission policy includes and re-exports it, so existing
// includers of application/permission_policy.h keep compiling unchanged.
enum class Command {
    Reset,          // 复位 (M103 pulse: clears fault/home-complete/width state)
    HomeStart,      // 回原点 (M50=1: starts PLC homing; user decision 2026-09-21)
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
    Count           // "no command" sentinel (idle status)
};

} // namespace hlm
