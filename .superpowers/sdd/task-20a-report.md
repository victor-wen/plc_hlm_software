# Task 20a Report: Pulse state machine + M112 watchdog wiring, comm stats

## Status: DONE — both build suites 100% green

## What changed

### `src/ports/iplc_gateway.h`
- Added `virtual bool startPulse(quint16 address) = 0;` (spec §8.5). Both
  concrete gateways implement it.

### `src/adapters/modbus/qt_modbus_plc_gateway.h/.cpp`
- `ModbusGatewayWorker` now owns `PulseStateMachine m_pulses` and
  `WatchdogTimer m_watchdog` as members, constructed with callbacks + injected
  clock. Callbacks are built by static factories `makePulseCallbacks` /
  `makeWatchdogCallbacks` (avoids the fragile nested-brace mem-initializer).
- Pulse `writeCoil` callback routes through `m_queue.enqueue`; a rejected
  enqueue (queue closed = offline) returns false → the machine aborts the
  pulse (spec §8.4 no-replay). `readCoil` enqueues a dedicated single-coil
  pulse readback (`enqueuePulseReadback`, matched by `isReadback`+`requestId`
  in `m_pulseReadbacks`, NOT the write-confirmation table). `finished` is a
  no-op by design: the pulse outcome reaches the UI via `writeCompleted`
  (write-0 readback confirmation = success, failed-write path = failure).
- `handleReadResult` routes pulse readbacks first (`handlePulseReadback`),
  then the existing write-confirmation path. Failed/send-failed pulse
  readbacks converge the machine as bit 0 and drop the entry (no leak).
- `submitWriteCoil`/`submitWriteRegister` and both write-failure paths feed
  `m_pulses.onWriteCompleted(address, ok)` (only active addresses react).
- `startPulse` slot calls `m_pulses.startPulse(addr)`; the facade uses
  `BlockingQueuedConnection` so the UI thread gets the synchronous accept/
  reject answer.
- `onPollTick` drives `m_pulses.onTick()` and `m_watchdog.onTick()` (50 ms
  tick; watchdog flips M112 at Heartbeat priority every 500 ms while online).
- `setOnline` follows connection state: `openLink` → false, first full
  snapshot → true, `enterOffline`/`stop` → false. `stop()`/`enterOffline()`
  call `m_pulses.reset()` and clear `m_pulseReadbacks`.
- Comm stats: `m_reconnectCount` incremented in `scheduleReconnect`,
  `m_failedPolls` incremented per exhausted poll retry; `publishSnapshot`
  emits `commStatsChanged(sequence, reconnectCount, failedPolls)`. Facade
  re-emits.
- Test hook `setWatchdogEnabled(bool)` (default true) so existing tests keep
  their exact dispatch-order assumptions; heartbeat tests re-enable it.

### `src/adapters/simulator/simulated_plc_gateway.h/.cpp`
- `startPulse(address)`: offline → rejected (writeCompleted false, returns
  false); online → writes true+false, confirms by readback, returns true.
- `commStatsChanged` signal emitted in `publishSnapshot`; `reconnectCount`
  increments on link-restore and freeze-recovery, `failedPolls` always 0.

### Tests
- `tests/unit/test_gateway.cpp`: 8 new tests — `startPulseRoutesThroughStateMachine`,
  `pulseHoldThenClearAtMin100ms`, `pulseClearPriorityLevel1`,
  `pulseAbortsOnOffline`, `uncertainPulseSetConvergesViaReadback`,
  `heartbeatFlipsM112Every500ms`, `heartbeatStopsOffline`, `commStatsEmitted`.
  Existing tests unchanged except init() disables the watchdog.
- `tests/integration/test_simulated_gateway_flow.cpp`: 2 new tests —
  `startPulseWritesCoilPair`, `commStatsEmitted`.

## Verification
- `cmake --build build && ctest --test-dir build --output-on-failure`:
  30/30 passed.
- `cmake --build build-vision && ctest --test-dir build-vision --output-on-failure`:
  31/31 passed.

## Notes / decisions
- Pulse `finished` callback is a no-op (brief's design decision): the
  coordinator converges on the snapshot; `writeCompleted` carries the pulse
  outcome.
- Pulse writes go through write-then-readback like all other writes, so the
  request stream for a pulse is: write-1, readback, (≥100 ms) write-0,
  readback. Tests account for this.
- `startPulse` on the real gateway is `BlockingQueued` (synchronous result);
  the worker slot itself is cheap (no I/O).
