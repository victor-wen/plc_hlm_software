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
# requested; later tasks add Widgets/SerialBus/SerialPort/Sql as needed.
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Test)

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
    src/domain/address_table.cpp
    src/domain/fault_code.cpp
    src/domain/device_snapshot.cpp
)
target_include_directories(hlm_core PUBLIC src)
target_link_libraries(hlm_core PUBLIC
    Qt6::Core
    OpenSSL::Crypto
)
target_compile_definitions(hlm_core PUBLIC HLM_VERSION_STRING="${PROJECT_VERSION}")

# Planned targets added by later tasks (do not create empty targets now):
#   hlm_modbus    -> Qt6::SerialBus Qt6::SerialPort
#   hlm_sqlite    -> Qt6::Sql
#   hlm_simulator -> Qt6::Core
#   hlm_vision    -> OpenCV::core (guarded by HLM_ENABLE_VISION)
#   hlm_ui        -> Qt6::Widgets
#   hlm_app       -> all production modules above
#   plc_simulator -> Qt6::SerialBus Qt6::SerialPort Qt6::Widgets + hlm_simulator

# --- Tests ------------------------------------------------------------------
enable_testing()
add_subdirectory(tests/unit)
