# Sources and headers are found rather than listed: a new file in a layer
# should not need a line here as well. CONFIGURE_DEPENDS makes CMake re-run
# the glob when a directory changes, so adding a file does not need a manual
# reconfigure.
file(GLOB_RECURSE WOLANG_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
file(GLOB_RECURSE WOLANG_CORE_HEADERS CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/wolang/*.h"
    "${PROJECT_SOURCE_DIR}/include/wolang/*.inc")

add_library(wolang_core STATIC ${WOLANG_CORE_SOURCES})

target_sources(wolang_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}/include
    FILES ${WOLANG_CORE_HEADERS})

target_include_directories(wolang_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

wolang_configure_target(wolang_core)

add_library(wolang::core ALIAS wolang_core)
