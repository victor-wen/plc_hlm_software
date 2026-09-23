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
    // 测试信号 (user decision 2026-09-22): the four simulated neighbouring-
    // station handshake signals on the 手动 page, each a 100 ms pulse like
    // M101/M102/M103/M43. The addresses are NOT in the current PLC program
    // (M114-M117 are absent from the ladder); the PLC engineer adds the rungs
    // that consume them, so on an unchanged PLC these pulses are inert.
    SimUpstreamBoardIn,       // M114 模拟前站进板信号
    SimDownstreamBoardRequest,// M115 模拟后站要板信号
    SimUpstreamBoardRequest,  // M116 模拟前站要板请求信号
    SimDownstreamExitRequest, // M117 模拟后站出站请求信号
    // NOTE (user decision 2026-09-23): there is no command for M15 any more.
    // M15 拍照结束 is the HMI's ANSWER to the PLC, written only after a scan
    // cycle actually succeeded, and M11 相机触发中 is what the HMI READS to
    // start one. Pulsing M15 from the bench would forge a completion, so the
    // 手动 page no longer touches it; its 「模拟拍照结束」 button drives the same
    // cycle the PLC's M11 does, without writing any coil.
    ParameterChange,// 用户/通讯/参数设置
    LogoutClear,    // 注销时清除 M42/M106-M111 (internal, not gated)
    Count           // "no command" sentinel (idle status)
};

} // namespace hlm
