cmake_minimum_required(VERSION 3.14)
project(prad2hvmon VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

# ── Compile-time paths (auto-discovered, overridable at configure time) ──────
# Both are also overridable at runtime via $PRAD2HV_DATABASE_DIR /
# $PRAD2HV_RESOURCE_DIR.  prad2hv_setup.sh (installed next to the binary)
# sets them so the install tree can be relocated freely.
if(NOT DEFINED DATABASE_DIR)
    set(DATABASE_DIR "${CMAKE_SOURCE_DIR}/database")
endif()
get_filename_component(DATABASE_DIR "${DATABASE_DIR}" ABSOLUTE)

if(NOT DEFINED RESOURCE_DIR)
    set(RESOURCE_DIR "${CMAKE_SOURCE_DIR}/resources")
endif()
get_filename_component(RESOURCE_DIR "${RESOURCE_DIR}" ABSOLUTE)

# ── FetchContent dependencies ────────────────────────────────────────────────
include(FetchContent)

# nlohmann/json (header-only)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# fmt (already used in the original project)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        10.2.1
    GIT_SHALLOW    TRUE
)

# websocketpp (header-only)
FetchContent_Declare(websocketpp
    GIT_REPOSITORY https://github.com/zaphoyd/websocketpp.git
    GIT_TAG        0.8.2
    GIT_SHALLOW    TRUE
)

# standalone Asio (header-only, no Boost)
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(json fmt websocketpp asio)

# Asio doesn't provide a CMake target — create an interface library
add_library(asio_standalone INTERFACE)
target_include_directories(asio_standalone INTERFACE
    ${asio_SOURCE_DIR}/asio/include
)
target_compile_definitions(asio_standalone INTERFACE
    ASIO_STANDALONE
)

# websocketpp needs to find Asio headers
add_library(websocketpp_lib INTERFACE)
target_include_directories(websocketpp_lib INTERFACE
    ${websocketpp_SOURCE_DIR}
)
target_link_libraries(websocketpp_lib INTERFACE asio_standalone)

# ── CAEN HV wrapper library ─────────────────────────────────────────────────
set(CAEN_LIB_DIR "${CMAKE_SOURCE_DIR}/caen_lib")

add_library(caen_lib STATIC
    ${CAEN_LIB_DIR}/caen_channel.cpp
)
target_include_directories(caen_lib PUBLIC ${CAEN_LIB_DIR})

# Link against the vendor shared library.  The .so is shipped in caen_lib/
# because there is no system package on the target hosts; we install it to
# <prefix>/lib so the loader picks it up from LD_LIBRARY_PATH (set by
# prad2hv_setup.sh).
set(CAENHV_VENDOR_SO "${CAEN_LIB_DIR}/libcaenhvwrapper.so")
find_library(CAENHVWRAPPER_LIB caenhvwrapper
    PATHS ${CAEN_LIB_DIR} /usr/lib /usr/local/lib
    NO_DEFAULT_PATH
)
if(NOT CAENHVWRAPPER_LIB)
    if(EXISTS "${CAENHV_VENDOR_SO}")
        set(CAENHVWRAPPER_LIB "${CAENHV_VENDOR_SO}")
        message(STATUS "Using libcaenhvwrapper.so from ${CAEN_LIB_DIR}")
    else()
        message(WARNING "libcaenhvwrapper.so not found — link may fail")
    endif()
endif()
if(CAENHVWRAPPER_LIB)
    target_link_libraries(caen_lib PUBLIC ${CAENHVWRAPPER_LIB})
endif()

# ── zlib (required by websocketpp permessage-deflate compression) ─────────────
find_package(ZLIB REQUIRED)

# ── Daemon executable ────────────────────────────────────────────────────────
add_executable(prad2hvd
    src/main.cpp
    src/file_fault_logger.cpp
    src/file_op_logger.cpp
)

target_include_directories(prad2hvd PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CAEN_LIB_DIR}
)

target_compile_definitions(prad2hvd PRIVATE
    DATABASE_DIR="${DATABASE_DIR}"
    RESOURCE_DIR="${RESOURCE_DIR}"
)

target_link_libraries(prad2hvd PRIVATE
    caen_lib
    nlohmann_json::nlohmann_json
    fmt::fmt
    websocketpp_lib
    asio_standalone
    ZLIB::ZLIB
    pthread
)

# Embed $ORIGIN/../lib in the installed binary so the loader finds
# libcaenhvwrapper.so even if the vendor .so lacks a proper DT_SONAME.
# prad2hv_setup.sh also sets LD_LIBRARY_PATH as a belt-and-suspenders.
set_target_properties(prad2hvd PROPERTIES
    INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

# ── Optional Qt GUI client ───────────────────────────────────────────────────
# Build with:  cmake .. -DBUILD_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt5/lib/cmake
# Skip with:   cmake ..   (default: OFF)
option(BUILD_GUI "Build the Qt WebEngine GUI client (prad2hvmon)" OFF)

if(BUILD_GUI)
    find_package(Qt5 REQUIRED COMPONENTS Widgets WebEngineWidgets WebSockets)

    add_executable(prad2hvmon
        src/gui_main.cpp
    )

    target_include_directories(prad2hvmon PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )

    target_compile_definitions(prad2hvmon PRIVATE
        RESOURCE_DIR="${RESOURCE_DIR}"
        DATABASE_DIR="${DATABASE_DIR}"
    )

    target_link_libraries(prad2hvmon PRIVATE
        Qt5::Widgets
        Qt5::WebEngineWidgets
        Qt5::WebSockets
    )

    message(STATUS "Qt GUI client: ON (resources: ${RESOURCE_DIR})")
else()
    message(STATUS "Qt GUI client: OFF (enable with -DBUILD_GUI=ON)")
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Install
# ─────────────────────────────────────────────────────────────────────────────
# Layout:
#   <prefix>/bin/                        prad2hvd, prad2hvmon,
#                                        prad2hv_setup.sh/.csh,
#                                        Python tool wrappers
#   <prefix>/lib/                        libcaenhvwrapper.so
#   <prefix>/share/prad2hvmon/database/  crates, hycal_modules, gui_config, …
#   <prefix>/share/prad2hvmon/resources/ monitor.html + css/js
#   <prefix>/share/prad2hvmon/scripts/   Python utilities (faultgrep, …)
install(TARGETS prad2hvd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_GUI)
    install(TARGETS prad2hvmon
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Vendor shared lib — required at runtime by prad2hvd
if(EXISTS "${CAENHV_VENDOR_SO}")
    install(FILES ${CAENHV_VENDOR_SO}
        DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(DIRECTORY database/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prad2hvmon/database
    PATTERN "fault_log"    EXCLUDE
    PATTERN "op_log"       EXCLUDE
    PATTERN "settings_log" EXCLUDE
    PATTERN "hv_settings"  EXCLUDE
    PATTERN "screenshots"  EXCLUDE
    PATTERN "vmon_data"    EXCLUDE
    PATTERN "prad1"        EXCLUDE
    PATTERN "prad2hvd.pid" EXCLUDE)

install(DIRECTORY resources/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prad2hvmon/resources)

# Python tool sources — wrappers go in bin/ (see below).  prad2hv_setup.sh
# and prad2hv_setup.csh.in also live here but are installed separately
# into bin/.
install(DIRECTORY scripts/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/prad2hvmon/scripts
    FILES_MATCHING
        PATTERN "*.py"
        PATTERN "README.md"
        PATTERN "__pycache__"     EXCLUDE
        # Template scripts with site-specific hard-coded paths — meant to be
        # copied + tweaked, never installed verbatim.
        PATTERN "start_prad2hvd"  EXCLUDE)

# prad2hv_setup.csh is generated so @CMAKE_INSTALL_PREFIX@ can be baked in
# as a fallback (csh has no BASH_SOURCE equivalent under nested `source`).
# prad2hv_setup.sh uses BASH_SOURCE and is installed as-is.
# Prefix distinguishes us from prad2evviewer's setup.sh/.csh when both
# are installed to the same <prefix>.
configure_file(${CMAKE_SOURCE_DIR}/scripts/prad2hv_setup.csh.in
    ${CMAKE_BINARY_DIR}/bin/prad2hv_setup.csh @ONLY)
install(FILES scripts/prad2hv_setup.sh ${CMAKE_BINARY_DIR}/bin/prad2hv_setup.csh
    DESTINATION ${CMAKE_INSTALL_BINDIR})

# Thin bin/ wrappers so Python tools run without sourcing prad2hv_setup.sh.
# Each
# wrapper exports PRAD2HV_DATABASE_DIR / PRAD2HV_RESOURCE_DIR and execs the
# installed script.
set(_PRAD2HV_PY_WRAPPERS
    "faultgrep:faultgrep.py"
    "merge_settings:merge_settings.py"
    "json2table:json2table.py"
    "caenhv_reset:caenhv_reset.py"
    "vmon_reader:vmon_reader.py"
    "prad2hv_post_elog:post_elog.py"
)
foreach(_entry ${_PRAD2HV_PY_WRAPPERS})
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _name)
    list(GET _parts 1 PRAD2HV_WRAPPER_SCRIPT_REL)
    configure_file(
        ${CMAKE_SOURCE_DIR}/cmake/prad2hv_py_wrapper.sh.in
        ${CMAKE_BINARY_DIR}/bin/${_name}
        @ONLY
    )
    install(PROGRAMS ${CMAKE_BINARY_DIR}/bin/${_name}
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endforeach()
unset(PRAD2HV_WRAPPER_SCRIPT_REL)

# ─────────────────────────────────────────────────────────────────────────────
#  Summary
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "----------------------------------------------------")
message(STATUS "  ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install pfx  : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Database     : ${DATABASE_DIR}")
message(STATUS "  Resources    : ${RESOURCE_DIR}")
message(STATUS "  Qt GUI       : ${BUILD_GUI}")
message(STATUS "----------------------------------------------------")
