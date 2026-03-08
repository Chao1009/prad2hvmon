cmake_minimum_required(VERSION 3.10)
project(PRadHVSystem)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Output directories ────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

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
add_executable(PRadHVSystem
    src/PRadHVSystem.cpp
)
target_include_directories(PRadHVSystem PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CAEN_LIB_DIR}
)
target_link_libraries(PRadHVSystem PRIVATE
    caen_channel
    caenhvwrapper
)

# ── RPATH: find libcaenhvwrapper.so at runtime ────────────────────────────────
# Embed the library's location so the binary runs without LD_LIBRARY_PATH
set_target_properties(PRadHVSystem PROPERTIES
    BUILD_RPATH   ${CAEN_LIB_DIR}
    INSTALL_RPATH ${CAEN_LIB_DIR}
    INSTALL_RPATH_USE_LINK_PATH ON
)

