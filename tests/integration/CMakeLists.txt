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

# --- PLC-HMI-006: restricted-mode hold release + target-width envelope --------

add_executable(test_plc_hmi_006_release_and_envelope plc_hmi_006_release_and_envelope_test.cpp)
target_link_libraries(test_plc_hmi_006_release_and_envelope PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_hmi_006_release_and_envelope COMMAND test_plc_hmi_006_release_and_envelope)
set_tests_properties(plc_hmi_006_release_and_envelope PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# CI-only timeout budget for the Debug-slow composition-root targets (PLC-HMI-006
# CI triage, run 35160221139): serial_settings_application passed at 59.16 s and
# seven other targets here measured 16-24 s under the shared
# `ctest ... --timeout 60` on a contended windows-2022 runner, so a normal
# execution-time fluctuation kills a correct test with no assertion output. A
# per-test TIMEOUT property takes precedence over the command-line --timeout and
# adds no retry, so genuine errors still surface (only full_flow is retried by
# policy). full_flow keeps its own global budget; its Debug retry path is
# unchanged.
# --- PLC-HMI-007: S-INJECT generation rejection + cancelled-manual lock -------

add_executable(test_plc_hmi_007_generation_rejection plc_hmi_007_generation_rejection_test.cpp)
target_link_libraries(test_plc_hmi_007_generation_rejection PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_hmi_007_generation_rejection COMMAND test_plc_hmi_007_generation_rejection)
set_tests_properties(plc_hmi_007_generation_rejection PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

add_executable(test_plc_hmi_007_cancelled_manual plc_hmi_007_cancelled_manual_test.cpp)
target_link_libraries(test_plc_hmi_007_cancelled_manual PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_hmi_007_cancelled_manual COMMAND test_plc_hmi_007_cancelled_manual)
set_tests_properties(plc_hmi_007_cancelled_manual PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

add_executable(test_plc_hmi_007_cancelled_manual_developer plc_hmi_007_cancelled_manual_developer_test.cpp)
target_link_libraries(test_plc_hmi_007_cancelled_manual_developer PRIVATE
    hlm_app_core
    hlm_simulator
    hlm_sqlite
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_hmi_007_cancelled_manual_developer COMMAND test_plc_hmi_007_cancelled_manual_developer)
set_tests_properties(plc_hmi_007_cancelled_manual_developer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

set_tests_properties(operator_command_application plc_m112_d140_session serial_settings_application serial_settings_application_developer restricted_mode_routing restricted_routing_developer plc_hmi_005_authoritative_values plc_hmi_006_release_and_envelope plc_hmi_007_generation_rejection plc_hmi_007_cancelled_manual plc_hmi_007_cancelled_manual_developer PROPERTIES
    TIMEOUT 240
)
