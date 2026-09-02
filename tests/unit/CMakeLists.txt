# Unit tests (spec §15.2). Task 1 adds the smoke test; later tasks add
# per-module test executables here.

add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE
    hlm_core
    Qt6::Test
)

add_test(NAME smoke COMMAND test_smoke)
