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

# --- Task 9: vision adapter (spec §6, §7.4, §13) ----------------------------
# Guarded by HLM_ENABLE_VISION: with the option OFF the hlm_vision target and
# this test are simply not built (verified by a scratch configure).

if(HLM_ENABLE_VISION)
    add_executable(test_vision_adapter test_vision_adapter.cpp)
    target_link_libraries(test_vision_adapter PRIVATE
        hlm_vision
        hlm_simulator
        Qt6::Test
    )

    add_test(NAME vision_adapter COMMAND test_vision_adapter)
endif()

# --- Task 10: UI shell and shared industrial widgets (spec §11) --------------
# Widget tests need a QApplication (QTEST_MAIN) and run headless via
# QT_QPA_PLATFORM=offscreen (CI has no display).

add_executable(test_widgets test_widgets.cpp)
target_link_libraries(test_widgets PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME widgets COMMAND test_widgets)

add_executable(test_shell test_shell.cpp)
target_link_libraries(test_shell PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME shell COMMAND test_shell)

# --- Task 11: 总览 page (spec §9, §11.3) --------------------------------------

add_executable(test_overview_page test_overview_page.cpp)
target_link_libraries(test_overview_page PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME overview_page COMMAND test_overview_page)

# --- Task 12: 配方与调宽 page (spec §10.3, §11.3, §11.4) ------------------------

add_executable(test_recipe_width_page test_recipe_width_page.cpp)
target_link_libraries(test_recipe_width_page PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME recipe_width_page COMMAND test_recipe_width_page)

# --- Task 13: 手动控制 page (spec §10.7-§10.8, §11.3, §11.4) ------------------------

add_executable(test_manual_control_page test_manual_control_page.cpp)
target_link_libraries(test_manual_control_page PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME manual_control_page COMMAND test_manual_control_page)

# --- Task 14: 报警 page (spec §11.3, §12) ----------------------------------------

add_executable(test_alarm_page test_alarm_page.cpp)
target_link_libraries(test_alarm_page PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME alarm_page COMMAND test_alarm_page)

# --- Task 15: 操作记录 page (spec §11.3, §12) ----------------------------------------

add_executable(test_audit_log_page test_audit_log_page.cpp)
target_link_libraries(test_audit_log_page PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME audit_log_page COMMAND test_audit_log_page)

# --- Task 16: I/O 与诊断 page (spec §8.2, §9, §11.3, §13) ------------------------

add_executable(test_diagnostics_page test_diagnostics_page.cpp)
target_link_libraries(test_diagnostics_page PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME diagnostics_page COMMAND test_diagnostics_page)

# --- Task 17: 用户与设置 page (spec §8.1, §11.3-§11.5) ------------------------

add_executable(test_users_settings_page test_users_settings_page.cpp)
target_link_libraries(test_users_settings_page PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME users_settings_page COMMAND test_users_settings_page)

# --- Task 18: standalone RTU simulator backend (spec §14.3, §15.5) ----------

add_executable(test_rtu_simulator test_rtu_simulator.cpp)
target_link_libraries(test_rtu_simulator PRIVATE
    plc_simulator
    Qt6::Test
)

add_test(NAME rtu_simulator COMMAND test_rtu_simulator)

# --- Task 19: RTU simulator control panel (spec §14.3) ------------------------

add_executable(test_control_panel test_control_panel.cpp)
target_link_libraries(test_control_panel PRIVATE
    plc_simulator
    Qt6::Test
)

add_test(NAME control_panel COMMAND test_control_panel)

# --- PLC-HMI-001: operator command lifecycle black-box tests -----------------

add_executable(test_operator_command_lifecycle operator_command_lifecycle_test.cpp)
target_link_libraries(test_operator_command_lifecycle PRIVATE
    hlm_ui
    hlm_simulator
    Qt6::Test
)

add_test(NAME operator_command_lifecycle COMMAND test_operator_command_lifecycle)

add_executable(test_command_status_projection command_status_projection_test.cpp)
target_link_libraries(test_command_status_projection PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME command_status_projection COMMAND test_command_status_projection)

add_executable(test_command_feedback_pages command_feedback_pages_test.cpp)
target_link_libraries(test_command_feedback_pages PRIVATE
    hlm_ui
    Qt6::Test
)

add_test(NAME command_feedback_pages COMMAND test_command_feedback_pages)

# --- PLC-HMI-001: developer-owned lifecycle/regression tests (D12) -----------

add_executable(test_operator_lifecycle_developer operator_lifecycle_developer_test.cpp)
target_link_libraries(test_operator_lifecycle_developer PRIVATE
    hlm_ui
    hlm_simulator
    Qt6::Test
)

add_test(NAME operator_lifecycle_developer COMMAND test_operator_lifecycle_developer)

# --- PLC-HMI-002: independent recipe editor/result-feedback tests -----------

add_executable(test_recipe_editor_state recipe_editor_state_test.cpp)
target_link_libraries(test_recipe_editor_state PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME recipe_editor_state COMMAND test_recipe_editor_state)

add_executable(test_recipe_result_feedback recipe_result_feedback_test.cpp)
target_link_libraries(test_recipe_result_feedback PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME recipe_result_feedback COMMAND test_recipe_result_feedback)

# --- PLC-HMI-002: developer-owned recipe editor/result-feedback tests (D8) ---

add_executable(test_recipe_editor_developer recipe_editor_developer_test.cpp)
target_link_libraries(test_recipe_editor_developer PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME recipe_editor_developer COMMAND test_recipe_editor_developer)

# --- PLC-HMI-003: independent PLC data-path integrity tests -----------------
# All three use QTEST_GUILESS_MAIN (QCoreApplication), so they need no
# QT_QPA_PLATFORM=offscreen environment entry.

add_executable(test_plc_submission_identity plc_submission_identity_test.cpp)
target_link_libraries(test_plc_submission_identity PRIVATE
    hlm_simulator
    Qt6::Test
)
add_test(NAME plc_submission_identity COMMAND test_plc_submission_identity)

add_executable(test_plc_command_correlation plc_command_correlation_test.cpp)
target_link_libraries(test_plc_command_correlation PRIVATE
    hlm_simulator
    Qt6::Test
)
add_test(NAME plc_command_correlation COMMAND test_plc_command_correlation)

add_executable(test_plc_snapshot_quality plc_snapshot_quality_test.cpp)
target_link_libraries(test_plc_snapshot_quality PRIVATE
    hlm_simulator
    Qt6::Test
)
add_test(NAME plc_snapshot_quality COMMAND test_plc_snapshot_quality)

# --- PLC-HMI-003 rework 1: developer-owned stale-publication regression -------
# QTEST_GUILESS_MAIN (QCoreApplication): no offscreen environment needed.

add_executable(test_plc_stale_publication_developer plc_stale_publication_developer_test.cpp)
target_link_libraries(test_plc_stale_publication_developer PRIVATE
    hlm_modbus
    Qt6::Test
)
add_test(NAME plc_stale_publication_developer COMMAND test_plc_stale_publication_developer)

# --- PLC-HMI-004: passive serial discovery + atomic settings batch -------------
# QTEST_GUILESS_MAIN targets (no QApplication): no offscreen entry needed.

add_executable(test_serial_connection_settings serial_connection_settings_test.cpp)
target_link_libraries(test_serial_connection_settings PRIVATE
    hlm_core
    Qt6::Test
)
add_test(NAME serial_connection_settings COMMAND test_serial_connection_settings)

add_executable(test_serial_discovery_contract serial_discovery_contract_test.cpp)
target_link_libraries(test_serial_discovery_contract PRIVATE
    hlm_core
    Qt6::Test
)
add_test(NAME serial_discovery_contract COMMAND test_serial_discovery_contract)

# QTEST_MAIN widget target: add serial_settings_page to the offscreen
# set_tests_properties(...) list at the end of this file.
add_executable(test_serial_settings_page serial_settings_page_test.cpp)
target_link_libraries(test_serial_settings_page PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME serial_settings_page COMMAND test_serial_settings_page)

# --- PLC-HMI-004: developer-owned batch atomicity regression (D7) ------------
# QTEST_GUILESS_MAIN (QCoreApplication): no offscreen entry needed.

add_executable(test_serial_settings_batch_developer serial_settings_batch_developer_test.cpp)
target_link_libraries(test_serial_settings_batch_developer PRIVATE
    hlm_sqlite
    Qt6::Test
)
add_test(NAME serial_settings_batch_developer COMMAND test_serial_settings_batch_developer)

# --- PLC-HMI-008: independent touch-visible disabled-reason tests -----------
# QTEST_MAIN widget target: visible_disabled_reasons is in the offscreen list.

add_executable(test_visible_disabled_reasons visible_disabled_reasons_test.cpp)
target_link_libraries(test_visible_disabled_reasons PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME visible_disabled_reasons COMMAND test_visible_disabled_reasons)

# --- PLC-HMI-008: developer-owned visible-reason regression tests (D5) -------
# QTEST_MAIN widget target: visible_reasons_developer is in the offscreen list.

add_executable(test_visible_reasons_developer visible_reasons_developer_test.cpp)
target_link_libraries(test_visible_reasons_developer PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME visible_reasons_developer COMMAND test_visible_reasons_developer)

# --- PLC-HMI-005: PLC-authoritative simulator parity -------------------------
# QTEST_GUILESS_MAIN target (no QApplication): no offscreen entry needed.

add_executable(test_plc_hmi_005_simulator_parity plc_hmi_005_simulator_parity_test.cpp)
target_link_libraries(test_plc_hmi_005_simulator_parity PRIVATE
    hlm_simulator
    Qt6::Test
)
add_test(NAME plc_hmi_005_simulator_parity COMMAND test_plc_hmi_005_simulator_parity)

# --- PLC-HMI-006: independent responsive-envelope tests ----------------------
# QTEST_MAIN widget target: plc_hmi_006_responsive_envelope is in the offscreen
# set_tests_properties(...) list at the end of this file.

add_executable(test_plc_hmi_006_responsive_envelope plc_hmi_006_responsive_envelope_test.cpp)
target_link_libraries(test_plc_hmi_006_responsive_envelope PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_hmi_006_responsive_envelope COMMAND test_plc_hmi_006_responsive_envelope)

# --- PLC-HMI-006: developer-owned responsive reflow regression tests (D6) ----
# QTEST_MAIN widget target: plc_hmi_006_layout is in the offscreen
# set_tests_properties(...) list at the end of this file.

add_executable(test_plc_hmi_006_layout plc_hmi_006_layout_developer_test.cpp)
target_link_libraries(test_plc_hmi_006_layout PRIVATE
    hlm_ui
    Qt6::Test
)
add_test(NAME plc_hmi_006_layout COMMAND test_plc_hmi_006_layout)

set_tests_properties(widgets shell overview_page recipe_width_page manual_control_page alarm_page audit_log_page diagnostics_page users_settings_page control_panel operator_command_lifecycle command_status_projection command_feedback_pages operator_lifecycle_developer recipe_editor_state recipe_result_feedback recipe_editor_developer serial_settings_page visible_disabled_reasons visible_reasons_developer plc_hmi_006_responsive_envelope plc_hmi_006_layout PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_LOGGING_RULES=*.debug=false"
)

# CI-only timeout budget for the Debug-slow widget targets (PLC-HMI-006 CI
# triage, run 35160221139): the shared `ctest ... --timeout 60` is a wall-clock
# budget these Debug widget builds cannot meet on a contended windows-2022
# runner - shell and plc_hmi_006_responsive_envelope were killed at 60 s with no
# assertion output while the same targets pass in seconds in Release. A per-test
# TIMEOUT property takes precedence over the command-line --timeout and adds no
# retry, so genuine failures still surface (only full_flow is retried by policy).
set_tests_properties(shell auth_service command_status_projection manual_control_page visible_disabled_reasons plc_hmi_006_responsive_envelope plc_hmi_006_layout PROPERTIES
    TIMEOUT 240
)
