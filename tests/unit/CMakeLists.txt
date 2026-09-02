# Unit tests (spec §15.2). Task 1 adds the smoke test; later tasks add
# per-module test executables here.

add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE
    hlm_core
    Qt6::Test
)

add_test(NAME smoke COMMAND test_smoke)

add_executable(test_snapshot_decode test_snapshot_decode.cpp)
target_link_libraries(test_snapshot_decode PRIVATE
    hlm_core
    Qt6::Test
)

add_test(NAME snapshot_decode COMMAND test_snapshot_decode)

add_executable(test_h3u_simulation_model test_h3u_simulation_model.cpp)
target_link_libraries(test_h3u_simulation_model PRIVATE
    hlm_simulator
    Qt6::Test
)

add_test(NAME h3u_simulation_model COMMAND test_h3u_simulation_model)

add_executable(test_request_queue test_request_queue.cpp)
target_link_libraries(test_request_queue PRIVATE
    hlm_modbus
    Qt6::Test
)

add_test(NAME request_queue COMMAND test_request_queue)

add_executable(test_reconnect_policy test_reconnect_policy.cpp)
target_link_libraries(test_reconnect_policy PRIVATE
    hlm_modbus
    Qt6::Test
)

add_test(NAME reconnect_policy COMMAND test_reconnect_policy)

add_executable(test_gateway test_gateway.cpp)
target_link_libraries(test_gateway PRIVATE
    hlm_modbus
    Qt6::Test
)

add_test(NAME gateway COMMAND test_gateway)

add_executable(test_pulse_state_machine test_pulse_state_machine.cpp)
target_link_libraries(test_pulse_state_machine PRIVATE
    hlm_modbus
    Qt6::Test
)

add_test(NAME pulse_state_machine COMMAND test_pulse_state_machine)

add_executable(test_watchdog_timer test_watchdog_timer.cpp)
target_link_libraries(test_watchdog_timer PRIVATE
    hlm_modbus
    Qt6::Test
)

add_test(NAME watchdog_timer COMMAND test_watchdog_timer)

add_executable(test_permission_policy test_permission_policy.cpp)
target_link_libraries(test_permission_policy PRIVATE
    hlm_core
    Qt6::Test
)

add_test(NAME permission_policy COMMAND test_permission_policy)

add_executable(test_interlock_rules test_interlock_rules.cpp)
target_link_libraries(test_interlock_rules PRIVATE
    hlm_core
    Qt6::Test
)

add_test(NAME interlock_rules COMMAND test_interlock_rules)

add_executable(test_control_coordinator test_control_coordinator.cpp)
target_link_libraries(test_control_coordinator PRIVATE
    hlm_simulator
    Qt6::Test
)

add_test(NAME control_coordinator COMMAND test_control_coordinator)

# --- Task 8: SQLite, auth, recipes, alarms, audit, rolling logger ------------

add_executable(test_password_derivation test_password_derivation.cpp)
target_link_libraries(test_password_derivation PRIVATE
    hlm_core
    Qt6::Test
)

add_test(NAME password_derivation COMMAND test_password_derivation)

add_executable(test_migrations test_migrations.cpp)
target_link_libraries(test_migrations PRIVATE
    hlm_sqlite
    Qt6::Test
)

add_test(NAME migrations COMMAND test_migrations)

add_executable(test_auth_service test_auth_service.cpp)
target_link_libraries(test_auth_service PRIVATE
    hlm_sqlite
    Qt6::Test
)

add_test(NAME auth_service COMMAND test_auth_service)

add_executable(test_repositories test_repositories.cpp)
target_link_libraries(test_repositories PRIVATE
    hlm_sqlite
    Qt6::Test
)

add_test(NAME repositories COMMAND test_repositories)

add_executable(test_alarm_edge_detector test_alarm_edge_detector.cpp)
target_link_libraries(test_alarm_edge_detector PRIVATE
    hlm_sqlite
    Qt6::Test
)

add_test(NAME alarm_edge_detector COMMAND test_alarm_edge_detector)

add_executable(test_rolling_file_logger test_rolling_file_logger.cpp)
target_link_libraries(test_rolling_file_logger PRIVATE
    hlm_core
    Qt6::Test
)

add_test(NAME rolling_file_logger COMMAND test_rolling_file_logger)

add_executable(test_database_service test_database_service.cpp)
target_link_libraries(test_database_service PRIVATE
    hlm_sqlite
    Qt6::Test
)

add_test(NAME database_service COMMAND test_database_service)