add_library(cfparser STATIC
    src/ConfigObject.cpp
    src/ConfigOption.cpp
    src/ConfigParser.cpp
    src/ConfigValue.cpp
)

target_include_directories(cfparser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

