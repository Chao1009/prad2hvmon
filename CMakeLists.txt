cmake_minimum_required(VERSION 3.14)
project(prad2hvd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── DATABASE_DIR compile-time define ─────────────────────────────────────────
# Points to the directory containing crates.json, hycal_modules.json, etc.
# Override at cmake time:  cmake .. -DDATABASE_DIR=/path/to/database
if(NOT DEFINED DATABASE_DIR)
    set(DATABASE_DIR "${CMAKE_SOURCE_DIR}/database")
endif()
get_filename_component(DATABASE_DIR "${DATABASE_DIR}" ABSOLUTE)

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

# Link against the vendor shared library
find_library(CAENHVWRAPPER_LIB caenhvwrapper
    PATHS ${CAEN_LIB_DIR} /usr/lib /usr/local/lib
    NO_DEFAULT_PATH
)
if(NOT CAENHVWRAPPER_LIB)
    # Fallback: use the .so directly from caen_lib/
    if(EXISTS "${CAEN_LIB_DIR}/libcaenhvwrapper.so")
        set(CAENHVWRAPPER_LIB "${CAEN_LIB_DIR}/libcaenhvwrapper.so")
        message(STATUS "Using libcaenhvwrapper.so from ${CAEN_LIB_DIR}")
    else()
        message(WARNING "libcaenhvwrapper.so not found — link may fail")
    endif()
endif()
if(CAENHVWRAPPER_LIB)
    target_link_libraries(caen_lib PUBLIC ${CAENHVWRAPPER_LIB})
endif()

# ── Daemon executable ────────────────────────────────────────────────────────
add_executable(prad2hvd
    src/main.cpp
    src/file_fault_logger.cpp
)

target_include_directories(prad2hvd PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CAEN_LIB_DIR}
)

target_compile_definitions(prad2hvd PRIVATE
    DATABASE_DIR="${DATABASE_DIR}"
)

target_link_libraries(prad2hvd PRIVATE
    caen_lib
    nlohmann_json::nlohmann_json
    fmt::fmt
    websocketpp_lib
    asio_standalone
    pthread
)
