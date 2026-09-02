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
