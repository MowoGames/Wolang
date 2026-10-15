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

# The runtime on its own, as something an ahead-of-time binary can link
# against. The same files the compiler itself holds: a program built with
# `wolang build` and one run by `wolang run` share an allocator, a global
# table and a class table, which is what makes a global written in one
# tier the one read in the other (D98).
file(GLOB WOLANG_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/*.cc")
add_library(wolang_runtime STATIC ${WOLANG_RUNTIME_SOURCES})
target_include_directories(wolang_runtime PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
wolang_configure_target(wolang_runtime)
add_library(wolang::runtime ALIAS wolang_runtime)

# Sockets and the loader are libraries of their own on Windows, and part
# of libc everywhere else (D159).
if(WIN32)
    target_link_libraries(wolang_runtime PUBLIC ws2_32)
else()
    find_library(WOLANG_DL_LIB dl)
    if(WOLANG_DL_LIB)
        target_link_libraries(wolang_runtime PUBLIC ${WOLANG_DL_LIB})
    endif()
endif()

target_sources(wolang_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}/include
    FILES ${WOLANG_CORE_HEADERS})

target_include_directories(wolang_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(WIN32)
    target_link_libraries(wolang_core PUBLIC ws2_32)
endif()

# The ahead-of-time backend, when LLVM is available. Through the C API
# rather than the C++ one: this project builds without RTTI and LLVM's
# C++ headers want it (D98).
if(WOLANG_WITH_LLVM)
    target_compile_definitions(wolang_core PUBLIC WOLANG_WITH_LLVM=1)
    target_include_directories(wolang_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    llvm_map_components_to_libnames(WOLANG_LLVM_LIBS
        core target native analysis passes bitwriter)
    target_link_libraries(wolang_core PUBLIC ${WOLANG_LLVM_LIBS})
endif()

# Where the runtime archive ends up, so that `wolang build` can hand it
# to the linker without being told.
target_compile_definitions(wolang_core PRIVATE
    WOLANG_RUNTIME_ARCHIVE="$<TARGET_FILE:wolang_runtime>"
    # Where the standard library is in this tree, so that `wolang new`
    # can point a package at it. An installed compiler finds it next to
    # itself instead (D132).
    WOLANG_LIBRARY_DIRECTORY="${PROJECT_SOURCE_DIR}/lib")
add_dependencies(wolang_core wolang_runtime)

wolang_configure_target(wolang_core)

add_library(wolang::core ALIAS wolang_core)

# Wolang as something to embed: one C header, one library to link.
#
# Static *and* shared, because the two hosts this is for want different
# things -- an engine built with the same toolchain links the archive, and
# a program that loads scripts at run time wants a library it can ship
# beside them (D128).
add_library(wolang_embed STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/embed/embed.cc)
target_link_libraries(wolang_embed PUBLIC wolang_core)
target_include_directories(wolang_embed PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
wolang_configure_target(wolang_embed)
add_library(wolang::embed ALIAS wolang_embed)

add_library(wolang_embed_shared SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/embed/embed.cc)
set_target_properties(wolang_embed_shared PROPERTIES
    OUTPUT_NAME wolang_embed
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_link_libraries(wolang_embed_shared PRIVATE wolang_core)
target_include_directories(wolang_embed_shared PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
wolang_configure_target(wolang_embed_shared)
