cmake_minimum_required(VERSION 3.11)
project(prad2hvmon)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output directories ────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── Qt5 ───────────────────────────────────────────────────────────────────────
# If Qt5 is not on the default path, point cmake to it:
#   cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt5/lib/cmake
find_package(Qt5 REQUIRED COMPONENTS
    Core
    Widgets
    WebEngineWidgets   # QWebEngineView, QWebEnginePage, etc.
    WebChannel         # C++ <-> JS bridge (QWebChannel)
)

# Required for Qt's code-generation tools (moc, uic, rcc)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ── fmt ───────────────────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG        e69e5f977d458f2650bb346dadf2ad30c5320281) # 10.2.1
FetchContent_MakeAvailable(fmt)

# ── cfparser static library ──────────────────────────────────────────────────
add_subdirectory(cfparser)

# ── Third-party: CAENHVWrapper ────────────────────────────────────────────────
set(CAEN_LIB_DIR ${CMAKE_SOURCE_DIR}/caen_lib)

# Import the pre-built shared library so CMake knows about it
add_library(caenhvwrapper SHARED IMPORTED)
set_target_properties(caenhvwrapper PROPERTIES
    IMPORTED_LOCATION ${CAEN_LIB_DIR}/libcaenhvwrapper.so
    INTERFACE_INCLUDE_DIRECTORIES ${CAEN_LIB_DIR}
)

# ── caen_channel static helper library ───────────────────────────────────────
add_library(caen_channel STATIC
    caen_lib/caen_channel.cpp
)
target_include_directories(caen_channel PUBLIC
    ${CAEN_LIB_DIR}
)
target_link_libraries(caen_channel PUBLIC caenhvwrapper)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(prad2hvmon
    src/prad2hvmon.cpp
    src/hv_monitor.h      # listed so AUTOMOC processes the Q_OBJECT macro
)
target_include_directories(prad2hvmon PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CAEN_LIB_DIR}
)

# Pass the resource directory so the binary can locate monitor.html at runtime
target_compile_definitions(prad2hvmon PRIVATE
    RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
)

target_link_libraries(prad2hvmon PRIVATE
    caen_channel
    caenhvwrapper
    cfparser
    Qt5::Core
    Qt5::Widgets
    Qt5::WebEngineWidgets
    Qt5::WebChannel
    fmt::fmt
)

# Qt WebEngine requires a GUI application type on some platforms
set_target_properties(prad2hvmon PROPERTIES
    WIN32_EXECUTABLE OFF   # keep console output on Windows during dev
)

# ── RPATH: find libcaenhvwrapper.so at runtime ────────────────────────────────
set_target_properties(prad2hvmon PROPERTIES
    BUILD_RPATH   ${CAEN_LIB_DIR}
    INSTALL_RPATH ${CAEN_LIB_DIR}
    INSTALL_RPATH_USE_LINK_PATH ON
)

# ── Install the HTML dashboard alongside the binary ──────────────────────────
install(FILES
    resources/monitor.html
    resources/monitor.css
    resources/monitor.js
    resources/hycal_modules.json
    resources/gui_config.json
    resources/crates.json
        DESTINATION ${CMAKE_INSTALL_PREFIX}/resources)
