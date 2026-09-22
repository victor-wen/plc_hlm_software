# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Windows C++20/Qt 6 HMI (上位机) for an Inovance H3U PLC over Modbus RTU. A single-process
modular monolith using ports and adapters: Qt Widgets UI → application services → domain/ports
→ adapters (Modbus RTU, SQLite, simulator, optional OpenCV vision). Cancellation, homing,
width adjustment, barcode ingestion, and a next-station link are all modelled against a real
production PLC program supplied under `需求/`.

## Build & test

**Canonical platform is Windows MSVC 2022 + Ninja + vcpkg**, driven by
`.github/workflows/windows-build-test.yml` (Debug, Release, Package/deploy-deps, and a
`HLM_ENABLE_VISION=OFF` job). That CI run is the acceptance evidence; local Linux builds are
dev-loop only.

Local Linux dev loop (system Qt 6.4.2, no vcpkg, no MSVC — vision must be OFF):

```bash
bash .ai/validation/linux-configure.sh cmake-build-plc-hmi-001   # cmake -G Ninja, Debug, VISION=OFF
bash .ai/validation/linux-build.sh cmake-build-plc-hmi-001       # cmake --build -j
bash .ai/validation/linux-ctest.sh cmake-build-plc-hmi-001 -R <test-name> --output-on-failure
bash .ai/validation/linux-ctest.sh cmake-build-plc-hmi-001 --parallel 4 --timeout 60 -E full_flow --output-on-failure
bash .ai/validation/linux-ctest.sh cmake-build-plc-hmi-001 -R full_flow --repeat until-pass:2 --output-on-failure
```

`full_flow` is retried (`--repeat until-pass:2`) because its PBKDF2 login work starves under
parallel load on shared runners; do not add retries to other targets. `linux-ctest.sh` forces
`QT_QPA_PLATFORM=offscreen` for widget tests. `bash .ai/validation/record-exit.sh <cmd...>`
prints and preserves an exact exit code — use it when reporting evidence.

Windows commands (from `.ai/project-contract.yaml#quality_commands`, PowerShell with
`$env:VCPKG_ROOT`): `cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
-DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
-DHLM_ENABLE_VISION=ON`, then `cmake --build build-release --config Release`, then ctest, then
`scripts/package.ps1` and `scripts/check_deploy_deps.ps1` (`-VisionEnabled:$true|$false`).

Lint and static analysis are **not configured** and cannot gate until an architect selects a
target (contract P-4). Do not substitute compiler warnings for them.

Run the app: `hlm_app` (real Modbus RTU gateway, the default) or `hlm_app --sim` (in-process
simulator); `--db <path>` overrides the SQLite path (default `%ProgramData%\PLC-HLM\app.db`).
Standalone RTU simulator for a virtual COM pair: `plc_simulator` (built from
`tools/plc_simulator/`). Test procedure: `docs/testing/rtu-simulator-acceptance.md`.

## Architecture

CMake static libraries under `src/`, each a hard module boundary (see `CMakeLists.txt`):

- **`hlm_core`** — domain (`address_table`, `device_snapshot`, `fault_code`, `command`,
  `operator_command_status`, `password_derivation`), application services
  (`control_coordinator`, `interlock_rules`, `permission_policy`), ports
  (`iplc_gateway`, `ivision_service`, `iserial_port_discovery`, `repositories`). Qt Core +
  OpenSSL Crypto only.
- **`hlm_simulator`** — shared `H3uSimulationModel` + injectable `SimulationClock` +
  in-process `SimulatedPlcGateway`.
- **`hlm_modbus`** — real gateway: `QtModbusRtuSerialClient` on its worker thread, serialized
  `request_queue`, `pulse_state_machine`, `reconnect_policy`, passive `QSerialPortInfo`
  discovery.
- **`hlm_sqlite`** — `DatabaseService` worker thread (single `QSqlDatabase` connection),
  `migration_runner`, repositories, `AuthService`, `AlarmEdgeDetector`.
- **`hlm_vision`** — OpenCV adapter behind `IVisionService`, optional via
  `-DHLM_ENABLE_VISION=OFF`; isolated so vision failure never affects PLC control.
- **`hlm_ui`** — `MainWindow`, shell (`shell_model`, `top_bar`, `nav_panel`, `action_bar`,
  `alarm_banner`), page + page-model pairs, shared widgets.
- **`hlm_app_core`** — `Application` (composition root, `src/app/application.cpp`) and
  `LifecycleController`. Static so integration tests link the real root; `hlm_app` is only the
  entry point.
- **`plc_simulator`** / `plc_simulator_exe` — standalone RTU simulator + fault-injection panel.

Dependency direction is one-way: UI → application → ports ← adapters. UI contains no Modbus
addresses, SQL, or transport calls. Vendor/Qt transport types (`QModbusReply`, `QSerialPort`,
`QSqlDatabase`, `QWidget*`) never cross a port boundary. Every concrete resource is owned by
one thread; cross-thread traffic carries immutable values only.

`Application` wires everything: startup order is db → serial settings → gateway → vision →
session timer → `window.show()`; shutdown is lifecycle → gateway → database → vision (M100 is
never auto-cleared). It owns request correlation, gateway-generation filtering, command-status
projection into `ShellModel`, and gateway rebuild after an atomic serial-settings batch.

### The command lifecycle contract

This is the invariant the whole codebase is organized around, and the most common source of
regressions:

- Every user request immediately produces a **visible** `Accepted`/`Pending` or `Rejected`
  state; every accepted finite command converges to exactly **one** terminal state
  (`Succeeded`/`Failed`/`TimedOut`/`CommunicationsLost`/`GatewayReplaced`). Nothing returns
  silently — a duplicate or in-progress request must emit a visible rejection.
- No optimistic machine state: a command is never reported successful before authoritative PLC
  confirmation. The single declared exception is reset (see below).
- Machine commands project through `OperatorCommandStatus` into a persistent, non-modal shell
  status visible from every page; recipe/settings results are page-local but equally
  never-silent. Disabled controls must expose their reason as inline touch-visible text —
  a tooltip alone is not acceptable.
- Every submission carries a `request_id` plus `gateway_generation`; completions are matched by
  both. Application increments the generation before replacing a gateway, disconnects old
  signals first, and drops late events from older generations. No address-only or FIFO
  correlation.
- `ControlCoordinator` is the sole authority for adjust results; page models render, they do
  not re-derive terminal state.

### H3U PLC contract (authoritative = the ladder under `需求/plc程序`)

`src/domain/address_table.*` is the single source of address semantics — never use bare address
numbers in flow code. Key semantics that differ from older documentation, all confirmed against
the PLC program:

- **Coils**: M0 physical estop, M1 manual / M2 auto / M3 running, M14 latched fault, M50
  HMI-written sustained home-start, M61 home-complete (mirrors M9), M60 auto-ready (mirrors
  M8), M100 software estop (holding bit, never auto-cleared), M101/M102/M103/M43 pulses
  (write 1, hold ≥100 ms, write 0), M104 auto mode, M105 passthrough, M106–M109 manual
  width/ belt/gate, M110/M111 interlock bypass.
- **Registers**: D140 is the sole liveness/heartbeat evidence (1 s heartbeat); D122 belt speed
  (init 5000); D204 pulse-per-mm (init 128); D220 width speed (init 20, **clamped to 15**);
  D126/D127 = D220 × K1280 (never D204×D220 — that gate was removed); D128 target / D130
  current width; D210 signed delta; D136/D137 motion pulses; D138/D139 production count.
- **M112 does not exist** in the PLC program. Never write it, simulate it, or cite it as
  liveness. M12, M35, D124 are unsupported/reserved.
- **Known PLC defect**: `DMUL D210 D204 D136` in SBR_MANUALWIDTH overlaps the D138/D139
  production counter. The HMI must disclose that the count may be unreliable after automatic
  width adjustment; repair is owned by a PLC engineer, not this repo.
- **Reset (PLC-HMI-011)**: the M103 pulse completes first, then exactly one sustained M50=1
  write. M50 is never pulsed, never repeated, never replayed offline. The PLC clears M50; the
  HMI adds one best-effort M50=0 to the logout/session-timeout/shutdown clear. Reset reports
  `复位完成` exactly once, only after both correlated transport outcomes and ≥200 ms from M103
  submission — snapshot M50/M61/M14/D110 values never decide the result. M106/M107 require
  M61=1 and M50=0; M108/M109 do not require M61.

Pulse timing, block quality/age (fast/home stale at 1000 ms, command at 2000 ms, slow at
4000 ms), and the request-queue priority ladder live in `hlm_modbus`. Poll-block quality must
come from real transfer outcomes — never hard-code blocks Valid or age zero.

## Workflow artifacts (`.ai/`)

`.ai/` is untracked on disk but is the **process authority** for this repo. Read it before
changing code that touches a gated area:

- **`.ai/project-contract.yaml`** — approved architecture contract, interfaces, permissions,
  invariants, `forbidden_changes`, risk rules R0–R3, and the canonical quality commands. Writes
  are architect-only; treat it as read-only unless the user explicitly asks. An invariant
  relaxation or any permission/safety/address/schema boundary change is **R3**: it needs an
  explicit user approval of a revised contract before implementation resumes.
- **`.ai/workflow-state.yaml`** — durable build-controller state: current change, gate
  artifacts with hashes, CI runs, task graph, deferred gates.
- **`.ai/changes/<ID>.yaml`** — per-change deliverables, `allowed_files` (only these may be
  edited), bounded blast radius.
- **`.ai/test-ownership.yaml`** — hash-pinned registry of independent test files. Independent
  tests are read-only inputs: **never edit them**. A behavior change that invalidates one goes
  through an explicit owner correction, re-hash, and manifest update.
- **`.ai/test-briefs/`**, **`.ai/reports/`**, **`.ai/reviews/`**, **`.ai/validation/`** —
  behavior-only briefs, gate evidence with real exit codes, reviews, and the local dev-loop
  scripts. Claims in reports are not evidence; commands and exit codes are.

Only developer-owned tests may be added or edited. New test targets must be registered in
`tests/unit/CMakeLists.txt` or `tests/integration/CMakeLists.txt`, with `QTEST_MAIN` widget
targets added to the `QT_QPA_PLATFORM=offscreen` property list and slow Debug targets to the
`TIMEOUT 240` list.

**Deferred gates** — do not claim these as passing: the **M50-HARDWARE-GATE** (real H3U proof
that one M50=1 write after the M103 pulse starts homing; blocks real-PLC connection,
deployment, and field release), real-DPI SAT, and the PLC-side D138/D139 repair. Simulator
success never substitutes for hardware evidence.

## Conventions

- Domain values are immutable and transport-neutral; they are `Q_DECLARE_METATYPE`d for queued
  cross-thread delivery.
- Comments cite `spec §N` (referring to
  `docs/superpowers/specs/2026-09-02-plc-hmi-architecture-design.md`) and change IDs. New work
  should follow the same referencing so a decision can be traced back to its source.
- Do not edit or download the PLC project under `需求/plc程序` — it is read-only evidence and a
  sensitive path. `需求/扫码相关` (scanner archives) is reference-only: the HMI drives the scan
  through the vendor `BarcodeReaderTrigger.dll` (loaded by name at runtime, never committed) and
  appends each decoded barcode to the administrator-configured file; an unconfigured path stays
  visibly 未配置. The next-station link is reserved architecture only — no socket, register map,
  or transport may be added.
- Operator-facing strings are Chinese; code, identifiers, and comments are English.
