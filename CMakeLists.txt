# PLC HMI (上位机) for Inovance H3U PLC over Modbus RTU.
#
# Build baseline (Task 1). Later tasks add the module targets listed in
# spec §6 (hlm_modbus, hlm_sqlite, hlm_simulator, hlm_vision, hlm_ui,
# hlm_app, plc_simulator) under the src/ and tools/ trees.

cmake_minimum_required(VERSION 3.21)

project(plc_hmi
    VERSION 0.1.0
    DESCRIPTION "Windows Qt/C++ HMI for Inovance H3U PLC over Modbus RTU"
    LANGUAGES CXX)

# --- Global settings --------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# --- Options ----------------------------------------------------------------
# Independent option (spec §6): vision is optional and its init failure must
# never block PLC control startup. Default ON; disable when OpenCV is absent.
option(HLM_ENABLE_VISION "Build the hlm_vision module (requires OpenCV 4.x)" ON)

# --- Dependencies -----------------------------------------------------------
# Qt: >= 6.4 for local dev (Ubuntu 24.04 ships 6.4.2); CI pins 6.8.x via the
# vcpkg manifest (vcpkg.json). Only components used by current targets are
# requested; later tasks add Widgets/Sql as needed.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Test SerialBus SerialPort Sql Widgets)

# OpenSSL 3.x Crypto: password derivation in hlm_core (spec §17).
find_package(OpenSSL 3 REQUIRED)

if(HLM_ENABLE_VISION)
    # OpenCV 4.x, isolated in the hlm_vision target (spec §6, §17).
    message(STATUS "HLM_ENABLE_VISION=ON: locating OpenCV 4.x (hlm_vision)")
    find_package(OpenCV 4 REQUIRED COMPONENTS core)
else()
    message(STATUS "HLM_ENABLE_VISION=OFF: hlm_vision module disabled")
endif()

# --- Module targets (spec §6) -----------------------------------------------
# hlm_core: address table, snapshot decoding, domain rules, application
# services, port interfaces and password derivation. Qt Core + OpenSSL Crypto.
add_library(hlm_core STATIC
    src/common/version.cpp
    src/common/rolling_file_logger.cpp
    src/application/control_coordinator.cpp
    src/application/interlock_rules.cpp
    src/application/permission_policy.cpp
    src/domain/address_table.cpp
    src/domain/fault_code.cpp
    src/domain/device_snapshot.cpp
    src/domain/password_derivation.cpp
    src/ports/iplc_gateway.cpp
    src/ports/ivision_service.cpp
)
target_include_directories(hlm_core PUBLIC src)
target_link_libraries(hlm_core PUBLIC
    Qt6::Core
    OpenSSL::Crypto
)
target_compile_definitions(hlm_core PUBLIC HLM_VERSION_STRING="${PROJECT_VERSION}")

# hlm_simulator: shared H3U simulation state model + injectable clock
# (spec §14.1) and the in-process SimulatedPlcGateway (spec §14.2). Used by
# the standalone plc_simulator (Task 18) and by application/UI integration
# tests. Qt Core only.
add_library(hlm_simulator STATIC
    src/adapters/simulator/simulation_clock.cpp
    src/adapters/simulator/h3u_simulation_model.cpp
    src/adapters/simulator/simulated_plc_gateway.cpp
)
target_include_directories(hlm_simulator PUBLIC src)
target_link_libraries(hlm_simulator PUBLIC
    hlm_core
    Qt6::Core
)

# hlm_modbus: real PLC gateway over Modbus RTU (spec §6, §7.2, §8). The
# worker thread owns a QModbusRtuSerialClient; the request queue and reconnect
# policy are the unit-testable core.
add_library(hlm_modbus STATIC
    src/adapters/modbus/modbus_transport.cpp
    src/adapters/modbus/pulse_state_machine.cpp
    src/adapters/modbus/request_queue.cpp
    src/adapters/modbus/reconnect_policy.cpp
    src/adapters/modbus/watchdog_timer.cpp
    src/adapters/modbus/qt_modbus_plc_gateway.cpp
)
target_include_directories(hlm_modbus PUBLIC src)
target_link_libraries(hlm_modbus PUBLIC
    hlm_core
    Qt6::Core
    Qt6::SerialBus
    Qt6::SerialPort
)

# hlm_sqlite: database worker thread, migrations and repository
# implementations (spec §6, §7.3, §12). Qt Sql only; the worker thread owns
# the single QSqlDatabase connection.
add_library(hlm_sqlite STATIC
    src/adapters/sqlite/migration_runner.cpp
    src/adapters/sqlite/sqlite_repositories.cpp
    src/adapters/sqlite/auth_service.cpp
    src/adapters/sqlite/alarm_edge_detector.cpp
    src/adapters/sqlite/database_service.cpp
)
target_include_directories(hlm_sqlite PUBLIC src)
target_link_libraries(hlm_sqlite PUBLIC
    hlm_core
    Qt6::Core
    Qt6::Sql
)

# hlm_vision: IVisionService OpenCV adapter + version/matrix self-test
# (spec §6, §7.4, §13). OpenCV is isolated in this target; the port interface
# (src/ports/ivision_service.h) is OpenCV-free. The self-test runs on the
# adapter's own worker thread and its failure only marks the vision diagnostic
# red - PLC control is unaffected (spec §13).
if(HLM_ENABLE_VISION)
    add_library(hlm_vision STATIC
        src/adapters/vision/vision_service.cpp
    )
    target_include_directories(hlm_vision PUBLIC src)
    target_link_libraries(hlm_vision PUBLIC
        hlm_core
        Qt6::Core
        opencv_core
    )
endif()

# hlm_ui: Qt Widgets UI shell and shared industrial widgets (spec §6, §11).
# UI never touches Modbus or SQL directly; it binds to ShellModel which the
# app shell wires to the application layer.
add_library(hlm_ui STATIC
    src/ui/MainWindow.cpp
    src/ui/shell/shell_model.cpp
    src/ui/shell/top_bar.cpp
    src/ui/shell/alarm_banner.cpp
    src/ui/shell/nav_panel.cpp
    src/ui/shell/action_bar.cpp
    src/ui/pages/overview_model.cpp
    src/ui/pages/overview_page.cpp
    src/ui/pages/recipe_width_model.cpp
    src/ui/pages/recipe_width_page.cpp
    src/ui/pages/manual_control_model.cpp
    src/ui/pages/manual_control_page.cpp
    src/ui/pages/alarm_model.cpp
    src/ui/pages/alarm_page.cpp
    src/ui/pages/audit_log_model.cpp
    src/ui/pages/audit_log_page.cpp
    src/ui/pages/diagnostics_model.cpp
    src/ui/pages/diagnostics_page.cpp
    src/ui/pages/users_settings_model.cpp
    src/ui/pages/users_settings_page.cpp
    src/ui/dialogs/login_dialog.cpp
    src/ui/dialogs/admin_password_dialog.cpp
    src/ui/widgets/hold_button.cpp
    src/ui/widgets/status_light.cpp
    src/ui/widgets/value_display.cpp
    src/ui/widgets/permission_button.cpp
    src/ui/theme.qrc
)
target_include_directories(hlm_ui PUBLIC src)
target_link_libraries(hlm_ui PUBLIC
    hlm_core
    Qt6::Widgets
)

# Planned targets added by later tasks (do not create empty targets now):
#   hlm_app       -> all production modules above
#   plc_simulator -> Qt6::SerialBus Qt6::SerialPort Qt6::Widgets + hlm_simulator

# --- Tests ------------------------------------------------------------------
enable_testing()
add_subdirectory(tests/unit)
add_subdirectory(tests/integration)
