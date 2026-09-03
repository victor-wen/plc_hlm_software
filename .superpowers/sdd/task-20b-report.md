# Task 20b Report — Review Fixes

## Critical

1. **writeCompleted → coordinator wiring** (`src/app/application.cpp` `wireGateway`):
   Added `connect(gw, &IPlcGateway::writeCompleted, m_coordinator, &ControlCoordinator::onWriteCompleted)`.
   Without it the M43 pulse (control_coordinator.cpp:486-488) is never sent and every
   adjustWidth times out with "调宽等待超时".
   - New regression test `FullFlowTest::applicationAdjustWidthConverges` in
     `tests/integration/test_full_flow.cpp` drives the real composition root
     (`Application` with a temp DB path + simulated gateway): create admin → login →
     home via raw gateway → `applyAdjustRequested(300)` → asserts M44 + D130==300 and
     `commandResult(AdjustWidth, true)` + recipe page status "调宽完成".
   - Verified the test FAILS when the wiring connect is removed (M34 never rises) and
     PASSES with the fix. No new Application API was needed (existing inspection
     accessors `gateway()/database()/coordinator()/window()` sufficed).

## Important

2. **Serial load counter** (`handleSettingLoaded`): decrement `m_pendingSerialLoads`
   before the `!setting` early return, so a first run (missing keys) still reaches 0
   and fires `setSerialConfig` echo.
3. **Audit paging** (`listRecentAudit`): added `offset` parameter through
   `DatabaseService::listRecentAudit(limit, offset)` → `AuditRepository::recent(limit, offset)`
   → `SqliteAuditRepository::recent` (SQL `LIMIT ? OFFSET ?`). `requestMore` now pages
   by `m_auditLoadedCount`; `handleAuditLoaded` appends the full page and accumulates
   the count. Existing callers keep working via the default `offset = 0`.
   - Added offset-paging assertions to `RepositoryTest::auditAppendAndQuery`.

## Minor

4. **Parameter write routing**: replaced the single `m_pendingParamAddr` slot with a
   FIFO `QList<int> m_pendingParamAddrs` so rapid D122→D220 writes do not drop the
   first result.
5. **`QtModbusPlcGateway::stop`**: added `m_pulseReadbacks.clear()` for symmetry with
   `enterOffline()`.
6. **Idempotent shutdown**: `Application::shutdown()` guarded by `m_shutdownDone` so the
   aboutToQuit + destructor double call no longer re-issues M42/M106-M111 clears against
   a stopped gateway.
7. **commStats wiring**: added a comment noting a third gateway type needs a new branch.
8. **startPulse BlockingQueuedConnection**: already documented (line 201-202); no change.

## Tests

- `cmake --build build && ctest --test-dir build --output-on-failure`: 31/31 passed.
- `cmake --build build-vision && ctest --test-dir build-vision --output-on-failure`: 32/32 passed.
- New test `applicationAdjustWidthConverges` passes; confirmed it fails without fix #1.
