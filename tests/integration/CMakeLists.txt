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

# Qt's offscreen platform plugin is unstable for the full-application test on
# Windows runners: it can terminate without producing QtTest output in either
# configuration as the widget tree evolves. Exercise the Windows build through
# the native platform plugin used in production; non-Windows CI remains
# headless through offscreen.
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

# --- PLC-HMI-001: operator command routing integration black-box tests --------

add_executable(test_operator_command_application operator_command_application_test.cpp)
target_link_libraries(test_operator_command_application PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)

add_test(NAME operator_command_application COMMAND test_operator_command_application)
set_tests_properties(operator_command_application PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-001: developer-owned composition-root projection tests (D12) -----

add_executable(test_operator_command_projection_developer operator_command_projection_developer_test.cpp)
target_link_libraries(test_operator_command_projection_developer PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)

add_test(NAME operator_command_projection_developer COMMAND test_operator_command_projection_developer)
set_tests_properties(operator_command_projection_developer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-002: developer-owned recipe database routing tests (D8) ----------

add_executable(test_recipe_database_routing_developer recipe_database_routing_developer_test.cpp)
target_link_libraries(test_recipe_database_routing_developer PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)

add_test(NAME recipe_database_routing_developer COMMAND test_recipe_database_routing_developer)
set_tests_properties(recipe_database_routing_developer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-003: developer regression tests (D9) -----------------------------

add_executable(test_plc_correlation_developer plc_correlation_developer_test.cpp)
target_link_libraries(test_plc_correlation_developer PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_modbus
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_correlation_developer COMMAND test_plc_correlation_developer)
set_tests_properties(plc_correlation_developer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-003: independent M112-removal / D140 liveness session test ------
# QTEST_MAIN (QApplication) through the composition root; offscreen headless.

add_executable(test_plc_m112_d140_session plc_m112_d140_session_test.cpp)
target_link_libraries(test_plc_m112_d140_session PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_m112_d140_session COMMAND test_plc_m112_d140_session)
set_tests_properties(plc_m112_d140_session PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-004: serial discovery passivity + atomic settings batch -----------

add_executable(test_serial_settings_application serial_settings_application_test.cpp)
target_link_libraries(test_serial_settings_application PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Sql
    Qt6::Test
)
add_test(NAME serial_settings_application COMMAND test_serial_settings_application)
set_tests_properties(serial_settings_application PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-004: developer-owned composition-root regression tests (D7) ------
# Enumeration purity, in-flight duplicate rejection and real-gateway rebuild
# gating; the database write lock holds the save open for the ordering checks.

add_executable(test_serial_settings_application_developer serial_settings_application_developer_test.cpp)
target_link_libraries(test_serial_settings_application_developer PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_modbus
    hlm_sqlite
    hlm_ui
    Qt6::Sql
    Qt6::Test
)
add_test(NAME serial_settings_application_developer COMMAND test_serial_settings_application_developer)
set_tests_properties(serial_settings_application_developer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-008: restricted-mode production routing -------------------------

add_executable(test_restricted_mode_routing restricted_mode_routing_test.cpp)
target_link_libraries(test_restricted_mode_routing PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME restricted_mode_routing COMMAND test_restricted_mode_routing)
set_tests_properties(restricted_mode_routing PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-008: developer-owned restricted-routing regression tests (D5) ---

add_executable(test_restricted_routing_developer restricted_routing_developer_test.cpp)
target_link_libraries(test_restricted_routing_developer PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME restricted_routing_developer COMMAND test_restricted_routing_developer)
set_tests_properties(restricted_routing_developer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# --- PLC-HMI-005: authoritative values + reliability disclosure --------------

add_executable(test_plc_hmi_005_authoritative_values plc_hmi_005_authoritative_values_test.cpp)
target_link_libraries(test_plc_hmi_005_authoritative_values PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_hmi_005_authoritative_values COMMAND test_plc_hmi_005_authoritative_values)
set_tests_properties(plc_hmi_005_authoritative_values PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)
