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
