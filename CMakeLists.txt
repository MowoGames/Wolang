cmake_minimum_required(VERSION 3.24)

project(Wolang
    VERSION 0.1.0
    DESCRIPTION "Wolang - a statically typed, compiled language with gamedev-grade ergonomics"
    HOMEPAGE_URL "https://github.com/monntecc/wolang"
    LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    endif()
endif()

option(WOLANG_BUILD_TESTS "Build the Wolang test suite" ON)
option(WOLANG_BUILD_TOOLS "Build the wolang command line driver" ON)
option(WOLANG_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(WOLANG_ENABLE_SANITIZERS "Enable address/undefined sanitizers in Debug" OFF)

# Ahead-of-time compilation, through LLVM's C API.
#
# Optional on purpose: the dev tier is the one every test uses and the one
# a developer waits for, so the build has to work on a machine with no
# LLVM at all. Where LLVM is present, `wolang build` produces an object
# file and a binary that needs no JIT (D98).
option(WOLANG_WITH_LLVM "Build the ahead-of-time backend" ON)
if(WOLANG_WITH_LLVM)
    find_package(LLVM CONFIG QUIET)
    if(LLVM_FOUND)
        message(STATUS "Wolang: ahead-of-time backend with LLVM ${LLVM_PACKAGE_VERSION}")
    else()
        message(STATUS "Wolang: no LLVM found, building without the ahead-of-time backend")
        set(WOLANG_WITH_LLVM OFF)
    endif()
endif()

include(cmake/WolangCompilerOptions.cmake)

add_subdirectory(src)

if(WOLANG_BUILD_TOOLS)
    add_subdirectory(tools/wolang)
endif()

if(WOLANG_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif()

# What an installed Wolang is: the driver, the libraries it compiles
# against, the runtime archive `wolang build` hands to the linker, and the
# embedding header and library. The layout is the one `wolang new` looks
# for -- `<prefix>/bin/wolang` beside `<prefix>/lib/std` (D132).
if(PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)

    if(WOLANG_BUILD_TOOLS)
        install(TARGETS wolang_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(TARGETS wolang_runtime wolang_embed
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(TARGETS wolang_embed_shared
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY
            ${PROJECT_SOURCE_DIR}/lib/std
            ${PROJECT_SOURCE_DIR}/lib/http
            ${PROJECT_SOURCE_DIR}/lib/game
            ${PROJECT_SOURCE_DIR}/lib/core
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PATTERN "*.wo"
        PATTERN "wolang.toml")
    install(FILES ${PROJECT_SOURCE_DIR}/include/wolang/embed/wolang.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wolang/embed)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/editors
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wolang)
endif()

if(PROJECT_IS_TOP_LEVEL)
    message(STATUS "")
    message(STATUS "Wolang ${PROJECT_VERSION}")
    message(STATUS "  build type ........ ${CMAKE_BUILD_TYPE}")
    message(STATUS "  compiler .......... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "  target ............ ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
    message(STATUS "  tests ............. ${WOLANG_BUILD_TESTS}")
    message(STATUS "  tools ............. ${WOLANG_BUILD_TOOLS}")
    message(STATUS "  sanitizers ........ ${WOLANG_ENABLE_SANITIZERS}")
    message(STATUS "")
endif()
