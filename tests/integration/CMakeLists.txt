# Integration tests (spec §15.4). Task 6 adds the in-process
# SimulatedPlcGateway flow tests; later tasks add the full application
# integration tests here.

add_executable(test_simulated_gateway_flow test_simulated_gateway_flow.cpp)
target_link_libraries(test_simulated_gateway_flow PRIVATE
    hlm_simulator
    Qt6::Test
)

add_test(NAME simulated_gateway_flow COMMAND test_simulated_gateway_flow)

# Task 20d: full in-process integration flow (spec §15.4 stage 3). Links the
# real composition root (hlm_app_core) plus the simulator/sqlite/ui modules.
add_executable(test_full_flow test_full_flow.cpp)
target_link_libraries(test_full_flow PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)

add_test(NAME full_flow COMMAND test_full_flow)

# Qt's offscreen platform plugin is unstable for the Release full-application
# integration test on the Windows runner.  The exact same compiled executable
# passes when exercised through the native Windows platform plugin, which is
# also what production uses.  Keep offscreen for non-Windows CI hosts.
if(WIN32)
    set(HLM_FULL_FLOW_QPA_PLATFORM windows)
else()
    set(HLM_FULL_FLOW_QPA_PLATFORM offscreen)
endif()
set_tests_properties(full_flow PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${HLM_FULL_FLOW_QPA_PLATFORM};QT_LOGGING_RULES=*.debug=false"
    # Debug PBKDF2 work is deliberately expensive. Running this integration
    # test beside auth_service under ctest --parallel can starve its 5-second
    # async waits on shared hosted runners even though the flow is correct.
    RUN_SERIAL TRUE
)
