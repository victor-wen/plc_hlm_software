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