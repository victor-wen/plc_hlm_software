# PLC 上位机实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 Windows Qt/C++ PLC 上位机、双层 PLC 模拟器、本地数据与 GitHub Actions 构建测试流程，并在没有真实 PLC 时完成软件模拟验收。

**Architecture:** 单进程模块化单体，Qt Widgets 表示层通过应用服务访问领域端口，Modbus、SQLite、模拟器和 OpenCV 作为独立适配器。进程内模拟器与独立 RTU 模拟器共享一个 H3U 状态模型。

**Tech Stack:** C++20、CMake、Qt 6.8.x、Qt Widgets、Qt SerialBus、Qt Sql、OpenSSL 3.x、OpenCV 4.x、MSVC 2022 x64、GitHub Actions。

## Global Constraints

- 唯一设计依据：`docs/superpowers/specs/2026-09-02-plc-hmi-architecture-design.md`。
- 页面任务由 `@frontend` 完成；开始前必须阅读任务列出的项目需求文件和参考图。
- 其他编码任务由 `@coder` 完成。
- 每个任务先完成单元或自动化测试并通过，再交给 `@reviewer` 审阅。
- `@reviewer` 的高、中严重度问题必须清零；低严重度问题必须修复或记录接受原因。
- 修复后必须重跑受影响测试并再次交给 `@reviewer`。
- UI 不直接访问 Modbus 或 SQL，不显示未经 PLC 快照确认的乐观状态。
- 无真实 PLC 的 CI 使用进程内模拟器；虚拟 COM 的 RTU 验收只在本地 Windows 环境执行。
- 本仓库已在计划审核通过后初始化；后续编码仍须严格按任务、测试和 review 门禁执行。

## Primary References

- `docs/superpowers/specs/2026-09-02-plc-hmi-architecture-design.md`
- `需求/PLC上位机地址及要求.txt`
- `需求/补充.txt`
- `需求/界面布局参考/微信图片_20260828085210_39_112.jpg`
- `需求/界面布局参考/微信图片_20260828085211_40_112.jpg`
- `需求/界面布局参考/微信图片_20260828085212_41_112.jpg`
- `需求/界面布局参考/微信图片_20260828085213_42_112.jpg`
- `需求/界面布局参考/微信图片_20260828085214_43_112.jpg`
- `需求/界面布局参考/微信图片_20260828085215_44_112.jpg`
- `需求/界面布局参考/c05a763963636b54205a0c93f707b7c4.jpg`
- Qt SerialBus：`https://doc.qt.io/qt-6.8/qtserialbus-index.html`
- GitHub Actions：`https://docs.github.com/en/actions`

---

### Task 1: 仓库、CMake 与 GitHub Actions 基线

- [x] **Owner:** `@coder`
- **Goal:** 完善仓库基础，建立 CMake 目标、依赖清单、测试入口、`.gitignore` 和 Windows GitHub Actions Debug/Release 构建矩阵。
- **Files:** `CMakeLists.txt`、`vcpkg.json`、`.gitignore`、`.github/workflows/windows-build-test.yml`、`tests/unit/test_smoke.cpp`。
- **Tests:** CMake 配置测试、目标链接冒烟测试、`ctest` 发现测试；CI 执行 Windows MSVC 2022 Debug/Release 构建。
- **Acceptance:** `HLM_ENABLE_VISION` 可开关；Qt 6.8/OpenSSL 3/OpenCV 4 版本受控；GitHub Actions 可构建并运行冒烟测试；第三方 Action 固定到审核过的提交 SHA。
- **References:** spec §6、§15.1、§17；GitHub Actions 官方文档。
- **Review:** 测试通过后交 `@reviewer`。

### Task 2: 地址表、数据类型与设备快照

- [x] **Owner:** `@coder`
- **Goal:** 实现集中 `AddressTable`、故障码、数据质量和不可变 `DeviceSnapshot`。
- **Files:** `src/domain/address_table.*`、`device_snapshot.*`、`fault_code.*`、`quality.*`、`tests/unit/test_snapshot_decode.cpp`。
- **Tests:** D100/D103 位映射；D126/D127、D136/D137、D138/D139 字序；D210 有符号；D140 回绕；非法值质量。
- **Acceptance:** UI 和流程代码无裸地址；完整快照原子发布；未知故障码不崩溃。
- **References:** spec §8.2、§9、§10.3.1；主需求地址表。
- **Review:** 测试通过后交 `@reviewer`。

### Task 3: 共享 H3U 模拟状态模型

- [x] **Owner:** `@coder`
- **Goal:** 实现两个模拟器共用的 `H3uSimulationModel`，覆盖复位、调宽、自动流程、故障、D140 和 M112。
- **Files:** `src/adapters/simulator/h3u_simulation_model.*`、`simulation_clock.*`、`tests/unit/test_h3u_simulation_model.cpp`。
- **Tests:** M43/M44/M45 互斥、目标锁存、SUB/MUL 不覆盖产量、动态超时、回原点故障 8/9、调宽故障 10、M112 两秒清位、模式和急停。
- **Acceptance:** 时间可注入并显式推进；所有场景确定性运行；行为与 spec §10.3.1 一致。
- **References:** spec §10.3、§10.3.1、§14.1。
- **Review:** 测试通过后交 `@reviewer`。

### Task 4: Modbus 请求队列与真实 PLC 网关

- [x] **Owner:** `@coder`
- **Goal:** 实现 `IPlcGateway`、`QModbusRtuSerialClient` 工作线程、单请求队列、轮询、写优先、重连和数据快照发布。
- **Files:** `src/ports/iplc_gateway.h`、`src/adapters/modbus/qt_modbus_plc_gateway.*`、`request_queue.*`、`reconnect_policy.*`。
- **Tests:** 请求优先级、防饥饿、三次失败离线、D140 冻结、1/2/5 秒退避、重连不重放命令。
- **Acceptance:** Modbus 对象只在工作线程使用；9600/19200 配置有效；正常写压力下快速快照不超过 1 秒未更新。
- **References:** spec §7.2、§8.1-§8.4。
- **Review:** 测试通过后交 `@reviewer`。

### Task 5: 脉冲命令与 M112 心跳

- [x] **Owner:** `@coder`
- **Goal:** 实现 M101/M102/M103/M43 统一脉冲状态机和工作线程 M112 翻转。
- **Files:** `src/adapters/modbus/pulse_state_machine.*`、`watchdog_timer.*`、对应单元测试。
- **Tests:** 写 1 应答后保持至少 100 ms；清零最高优先级；不确定写不重复置 1；M112 每 500 ms 翻转且正常压力下应答间隔不超过 1 秒。
- **Acceptance:** 无阻塞 sleep；异常路径向安全状态收敛；离线不排队。
- **References:** spec §8.4-§8.6。
- **Review:** 测试通过后交 `@reviewer`。

### Task 6: 进程内 PLC 网关

- [x] **Owner:** `@coder`
- **Goal:** 实现包装共享模型的 `SimulatedPlcGateway`，供应用和 UI 测试替代真实 PLC。
- **Files:** `src/adapters/simulator/simulated_plc_gateway.*`、`tests/integration/test_simulated_gateway_flow.cpp`。
- **Tests:** 复位→调宽→自动→启动→停止，以及失败、超时、急停、断线和恢复场景。
- **Acceptance:** 与真实网关使用同一接口；测试不依赖串口或真实等待。
- **References:** spec §14.2、§15.4。
- **Review:** 测试通过后交 `@reviewer`。

### Task 7: 控制协调器与权限互锁

- [x] **Owner:** `@coder`
- **Goal:** 实现复位、配方调宽、模式、启动、停止、软件急停、手动命令和屏蔽的统一业务协调。
- **Files:** `src/application/control_coordinator.*`、`permission_policy.*`、`interlock_rules.*`、对应单元测试。
- **Tests:** 权限矩阵全部组合；每个流程的前置条件、超时和结果收敛；注销清 M42/M106-M111；M100 不自动清除。
- **Acceptance:** 所有写命令统一校验；启动等 M3、复位等 M61、调宽等 M44/M45；离线禁写。
- **References:** spec §10、§11.4、§13。
- **Review:** 测试通过后交 `@reviewer`。

### Task 8: SQLite、认证、配方、报警和审计

- [x] **Owner:** `@coder`
- **Goal:** 实现数据库线程、迁移、用户认证、配方、设置、报警边沿、审计、365 天清理和滚动诊断日志。
- **Files:** `src/adapters/sqlite/`、`src/ports/repositories.h`、`src/common/rolling_file_logger.*`、`tests/unit/test_*_repository.cpp`、`tests/unit/test_rolling_file_logger.cpp`。
- **Tests:** PBKDF2 向量、账号锁定、迁移回滚、唯一约束、报警开始/恢复、未知代码、保留期、数据库受限模式、10×10 MiB 日志轮转和敏感信息脱敏。
- **Acceptance:** 单线程单连接、WAL、日志脱敏、无明文密码；数据库失败时只保留在线停止和置软件急停；诊断日志遵守 spec 的数量、大小和脱敏限制。
- **References:** spec §7.3、§11.5、§12、§13。
- **Review:** 测试通过后交 `@reviewer`。

### Task 9: OpenCV 隔离适配器

- [x] **Owner:** `@coder`
- **Goal:** 实现 `IVisionService` 和 OpenCV 版本/矩阵自检，不进入 PLC 控制闭环。
- **Files:** `src/ports/ivision_service.h`、`src/adapters/vision/`、`tests/unit/test_vision_adapter.cpp`。
- **Tests:** 自检成功、失败和关闭构建选项；视觉失败时 PLC 控制仍可运行。
- **Acceptance:** OpenCV 类型不泄漏到领域层；运行失败只影响诊断状态。
- **References:** spec §6、§7.4、§13。
- **Review:** 测试通过后交 `@reviewer`。

### Task 10: UI 外壳与通用工业控件

- [x] **Owner:** `@frontend`
- **Goal:** 实现顶部状态、报警横幅、左导航、右操作栏、主题和共享触摸控件。
- **Files:** `src/ui/MainWindow.*`、`src/ui/shell/`、`src/ui/widgets/`、`src/ui/theme.qss`。
- **Tests:** 导航、权限禁用原因、过期值、HoldButton 全部释放路径、无乐观状态、100/125/150% DPI。
- **Acceptance:** 1920×1080 无裁剪；触摸目标≥48 px；软件急停固定红色并独立；页面切换和弹窗触发持续命令清零意图。
- **References:** spec §11.1-§11.2；`微信图片_20260828085215_44_112.jpg`、`微信图片_20260828085210_39_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 11: 总览页面

- [x] **Owner:** `@frontend`
- **Goal:** 展示设备状态、D120 步骤、宽度、差值、速度、产量和最新报警。
- **Files:** `src/ui/pages/OverviewPage.*`、页面模型和测试。
- **Tests:** 快照字段映射、过期显示“—”、只读页面不发送写意图。
- **Acceptance:** 完整快照更新；无逐字段或乐观更新；主要信息一屏可读。
- **References:** spec §9、§11.3；`微信图片_20260828085215_44_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 12: 配方与调宽页面

- [x] **Owner:** `@frontend`
- **Goal:** 实现配方 CRUD、宽度输入、应用确认、互锁原因和 M34/M44/M45 结果展示。
- **Files:** `src/ui/pages/RecipeWidthPage.*`、页面模型和测试。
- **Tests:** 选择配方不写 PLC；应用确认；50-400 校验；目标等于当前时只更新 D128、不发 M43；等待 PLC 结果。
- **Acceptance:** 操作员只读；失败和超时明确；D130 等于本次锁存目标才显示成功。
- **References:** spec §10.3、§11.3；`需求/补充.txt`、`微信图片_20260828085215_44_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 13: 手动控制页面

- [x] **Owner:** `@frontend`
- **Goal:** 实现点动、挡停、调宽速度、直通、常转和安全屏蔽界面。
- **Files:** `src/ui/pages/ManualControlPage.*`、页面模型和测试。
- **Tests:** 按下/释放/移出/失活/切页/弹窗/注销清零；权限、互锁、回读状态和离线禁用。
- **Acceptance:** 操作员只读；屏蔽二次确认并持续显示琥珀横幅；不根据按钮按下乐观显示执行成功。
- **References:** spec §10.7-§10.8、§11.3；`微信图片_20260828085212_41_112.jpg`、`微信图片_20260828085211_40_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 14: 报警页面

- [x] **Owner:** `@frontend`
- **Goal:** 实现当前/历史 PLC 与 HMI 报警、日期和代码筛选。
- **Files:** `src/ui/pages/AlarmPage.*`、过滤模型和测试。
- **Tests:** 活动/恢复、未知代码、筛选、异步加载和 48 px 行高。
- **Acceptance:** 不引入无地址支撑的确认语义；代码和中文含义完整显示。
- **References:** spec §11.3、§12；`微信图片_20260828085210_39_112.jpg`、`c05a763963636b54205a0c93f707b7c4.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 15: 操作记录页面

- [x] **Owner:** `@frontend`
- **Goal:** 展示并筛选时间、用户、角色、动作、对象、脱敏参数、结果和失败原因。
- **Files:** `src/ui/pages/AuditLogPage.*`、页面模型和测试。
- **Tests:** 筛选、异步分页/滚动、匿名用户显示和敏感字段不渲染。
- **Acceptance:** 页面只读且不阻塞 UI；不存在密码或未脱敏内容。
- **References:** spec §11.3、§12；`微信图片_20260828085213_42_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 16: I/O 与诊断页面

- [x] **Owner:** `@frontend`
- **Goal:** 展示原始状态字、已定义位、关键寄存器、通讯统计和 OpenCV 自检。
- **Files:** `src/ui/pages/DiagnosticsPage.*`、页面模型和测试。
- **Tests:** D100/D103 位展示、M50-M53/M100-M112、D140 活性、过期值和视觉失败隔离。
- **Acceptance:** D102/D104/D105 只显示原始值；页面不发送控制命令。
- **References:** spec §8.2、§9、§11.3；`微信图片_20260828085215_44_112.jpg`、`微信图片_20260828085211_40_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 17: 用户与设置页面

- [x] **Owner:** `@frontend`
- **Goal:** 实现登录、首次管理员、用户管理、会话提示、通讯配置和管理员参数设置。
- **Files:** `src/ui/pages/UsersSettingsPage.*`、`src/ui/dialogs/`、页面模型和测试。
- **Tests:** 未登录/操作员锁定、三次失败锁定、会话超时、D122 的 100-20000 范围、D204/D220 及频率乘积校验、注销清零意图。
- **Acceptance:** 无默认密码；敏感字段不向无权限用户渲染；D204 修改需再次验证管理员密码。
- **References:** spec §8.1、§11.4-§11.5；`微信图片_20260828085214_43_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 18: 独立 RTU 模拟器后端

- [x] **Owner:** `@coder`
- **Goal:** 使用 `QModbusRtuSerialServer` 包装共享 H3U 模型，提供站号、虚拟 COM 和故障注入接口。
- **Files:** `tools/plc_simulator/rtu_server.*`、`fault_injection.*`、对应测试。
- **Tests:** 功能码 01/03/05/06、站号过滤、寄存器映射、延迟/异常/断线注入。
- **Acceptance:** 不复制 PLC 业务逻辑；站号必须明确；无需虚拟 COM 也能完成后端单元测试。
- **References:** spec §14.3、§15.5。
- **Review:** 测试通过后交 `@reviewer`。

### Task 19: RTU 模拟器控制面板

- [x] **Owner:** `@frontend`
- **Goal:** 实现模拟器连接、状态、寄存器和故障场景控制面板。
- **Files:** `tools/plc_simulator/ControlPanel.*`、页面模型和测试。
- **Tests:** 场景选择只调用后端接口；状态回显、无效配置和触摸/鼠标操作。
- **Acceptance:** 可选择成功、条件失败、超时、故障码、断线、延迟、异常响应、非法值和心跳冻结；请求日志可读。
- **References:** spec §14.3；布局分组参考 `微信图片_20260828085212_41_112.jpg`、`微信图片_20260828085211_40_112.jpg`。
- **Review:** 测试通过后交 `@reviewer`。

### Task 20: 应用组合与进程内集成测试

- [x] **Owner:** `@coder`
- **Goal:** 装配线程、端口、适配器和全部 UI，完成阶段 3 进程内集成测试。
- **Files:** `src/app/application.*`、`configuration.*`、`lifecycle_controller.*`、`tests/integration/test_full_flow.cpp`。
- **Tests:** 首次启动、登录、完整生产流程、故障、权限、持久化、重连、受限模式、退出清理和 M100 保持。
- **Acceptance:** 线程归属正确；所有单元测试和集成测试通过；无真实 PLC 依赖。
- **References:** spec §7、§13、§15.4。
- **Review:** 测试通过后交 `@reviewer`。

### Task 21: CI 打包与模拟 PLC 验收门

- [x] **Owner:** `@coder`
- **Goal:** 完成 GitHub Actions 的全量 Windows 构建/测试/打包，并编写本地虚拟 COM RTU 验收步骤和报告模板。
- **Files:** `.github/workflows/windows-build-test.yml`、`scripts/package.ps1`、`scripts/check_deploy_deps.ps1`、`LICENSES/`、`docs/testing/rtu-simulator-acceptance.md`。
- **Tests:** CI 重跑全部单元和进程内集成测试；打包依赖检查故意移除 DLL 时必须失败；本地执行 9600/19200 RTU 场景。
- **Acceptance:** CI 产出可下载构建和测试报告；发行包包含 qwindows、qsqlite、SerialBus/SerialPort、OpenSSL、OpenCV 和许可证；GitHub Actions 不安装虚拟 COM 驱动；本地验收报告覆盖 spec §15.5。
- **References:** spec §15.1、§15.5、§17、§18；GitHub Actions 官方文档。
- **Review:** 全部测试和本地报告完成后交 `@reviewer` 做最终审阅。

## Dependency Order

```text
Task 1 → Task 2
Task 2 → Task 3、Task 4、Task 8、Task 9
Task 3 → Task 6、Task 18
Task 4 → Task 5、Task 6、Task 7、Task 18
Task 7、Task 8、Task 9 → Task 10
Task 10 → Task 11-Task 17、Task 19
Task 3-Task 19 → Task 20
Task 20 → Task 21
```

可并行任务必须使用独立 subagent，且不得同时修改共享接口文件。每个任务仍独立执行“测试 → `@reviewer` → 修复 → 回归测试 → 复审”闭环。

## Completion Criteria

- Task 1-Task 21 全部达到各自验收标准。
- 每个任务都有通过的单元或自动化测试和 `@reviewer` 审阅记录。
- GitHub Actions 的 Windows Debug/Release 构建、单元测试、进程内集成测试和打包检查通过。
- 本地虚拟 COM 的模拟 PLC RTU 验收通过并保存报告。
- 发行说明明确标记真实汇川 H3U 现场 SAT 尚未完成，投产前必须执行 spec §15.6。
