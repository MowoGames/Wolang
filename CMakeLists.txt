cmake_minimum_required(VERSION 3.24)

project(Wolang
    VERSION 0.1.0
    DESCRIPTION "Wolang - a statically typed, compiled language with gamedev-grade ergonomics"
    HOMEPAGE_URL "https://github.com/MowoGames/Wolang"
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

# This project is built with clang.
#
# Not a preference: the compiler uses clang's overflow builtins for the
# checked arithmetic every Wolang operator compiles to, `__int128` for
# the wide multiply a constant divisor turns into, and the statement
# expressions the trap machinery is written with. GCC has most of those
# and is untested; Microsoft's compiler has none of them.
#
# On Windows that means clang-cl, which is what the LLVM installer puts
# there and what the `windows` preset selects (D184).
option(WOLANG_REQUIRE_CLANG "Insist on clang" ON)
if(WOLANG_REQUIRE_CLANG AND
   NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "Wolang is built with clang, and this is "
        "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}.\n"
        "  macOS:   xcode-select --install   (Apple clang is fine)\n"
        "  Linux:   sudo apt install clang lld   then: "
        "CC=clang CXX=clang++ cmake --preset debug\n"
        "  Windows: install LLVM, then: cmake --preset windows   "
        "(that preset uses clang-cl)\n"
        "See docs/building.md. Pass -DWOLANG_REQUIRE_CLANG=OFF to try "
        "anyway.")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS
        "Wolang: ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION} at ${CMAKE_CXX_COMPILER}")
endif()

option(WOLANG_BUILD_TESTS "Build the Wolang test suite" ON)
option(WOLANG_BUILD_TOOLS "Build the wolang command line driver" ON)
option(WOLANG_BUILD_BENCHMARKS "Build the embedding benchmarks" ON)
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
    # LLVM's own config probes for libedit and zlib with C, and fails
    # when C is not a language of the project.
    enable_language(C)
    find_package(LLVM CONFIG QUIET)
    # Windows: LLVM's installer ships `LLVM-C.dll` but not the headers or a
    # CMake package, so the C API comes from a release archive instead --
    # `tools/package/fetch_llvm.py` puts one in `build/llvm-sdk` (D208).
    set(WOLANG_LLVM_C_ROOT "" CACHE PATH "An LLVM release with include/llvm-c, lib/LLVM-C.lib and bin/LLVM-C.dll")
    if(NOT LLVM_FOUND AND WIN32 AND NOT WOLANG_LLVM_C_ROOT)
        # 22, not 23: LLVM 23's IPSCCP folds `x != 6 % x` to false (D208).
        set(WOLANG_LLVM_C_VERSION "22.1.8")
        file(GLOB WOLANG_LLVM_C_FOUND "${PROJECT_SOURCE_DIR}/build/llvm-sdk/clang+llvm-${WOLANG_LLVM_C_VERSION}-*-windows-msvc")
        if(WOLANG_LLVM_C_FOUND)
            list(GET WOLANG_LLVM_C_FOUND 0 WOLANG_LLVM_C_FIRST)
            set(WOLANG_LLVM_C_ROOT "${WOLANG_LLVM_C_FIRST}" CACHE PATH "" FORCE)
        endif()
    endif()
    set(WOLANG_LLVM_C OFF)
    if(LLVM_FOUND)
        message(STATUS "Wolang: ahead-of-time backend with LLVM ${LLVM_PACKAGE_VERSION}")
    elseif(WIN32 AND EXISTS "${WOLANG_LLVM_C_ROOT}/include/llvm-c/Core.h" AND EXISTS "${WOLANG_LLVM_C_ROOT}/lib/LLVM-C.lib")
        set(WOLANG_LLVM_C ON)
        set(WOLANG_LLVM_C_DLL "${WOLANG_LLVM_C_ROOT}/bin/LLVM-C.dll")
        message(STATUS "Wolang: ahead-of-time backend with LLVM-C from ${WOLANG_LLVM_C_ROOT}")
    else()
        message(STATUS "Wolang: no LLVM found, building without the ahead-of-time backend")
        set(WOLANG_WITH_LLVM OFF)
    endif()
endif()

# Puts `LLVM-C.dll` beside a program that links it, which is where Windows
# looks first.
function(wolang_ship_llvm_c target)
    if(WOLANG_LLVM_C AND EXISTS "${WOLANG_LLVM_C_DLL}")
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WOLANG_LLVM_C_DLL}" "$<TARGET_FILE_DIR:${target}>")
    endif()
endfunction()

include(GNUInstallDirs)
include(cmake/WolangCompilerOptions.cmake)

add_subdirectory(src)

# The window, pixels and sound layer, built unless a build asks for a
# compiler and nothing else (D155).
option(WOLANG_BUILD_PLATFORM "Build the window, GPU and sound library" ON)
if(WOLANG_BUILD_PLATFORM)
    add_subdirectory(platform)
endif()

if(WOLANG_BUILD_TOOLS)
    add_subdirectory(tools/wolang)
endif()

if(WOLANG_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif()

# A host ticking actors through a script, against the same logic in C++
# (D219). Built with everything else so that it keeps compiling; timed
# from a release build.
if(WOLANG_BUILD_BENCHMARKS)
    add_executable(wolang_actor_bench benchmarks/actors/host.cc)
    target_link_libraries(wolang_actor_bench PRIVATE wolang_embed)
    wolang_configure_target(wolang_actor_bench)
    wolang_ship_llvm_c(wolang_actor_bench)
    # The same script compiled ahead of time and linked in, which is what a
    # shipped game runs, when this build has the backend to do it.
    if(WOLANG_BUILD_TOOLS AND (WOLANG_WITH_LLVM OR WOLANG_LLVM_C))
        set(WOLANG_ACTORS_OBJECT ${CMAKE_CURRENT_BINARY_DIR}/actors${CMAKE_CXX_OUTPUT_EXTENSION})
        add_custom_command(
            OUTPUT ${WOLANG_ACTORS_OBJECT}
            COMMAND wolang_cli build --library actors -O2 --no-debug ${PROJECT_SOURCE_DIR}/benchmarks/actors/actors.wo -o ${WOLANG_ACTORS_OBJECT}
            DEPENDS wolang_cli ${PROJECT_SOURCE_DIR}/benchmarks/actors/actors.wo
            COMMENT "Compiling the actor script ahead of time"
            VERBATIM)
        target_sources(wolang_actor_bench PRIVATE ${WOLANG_ACTORS_OBJECT})
        set_source_files_properties(${WOLANG_ACTORS_OBJECT} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        target_compile_definitions(wolang_actor_bench PRIVATE WOLANG_ACTORS_AOT=1)
    endif()
endif()

# What an installed Wolang is: the driver, the libraries it compiles
# against, the runtime archive `wolang build` hands to the linker, and the
# embedding header and library. The layout is the one `wolang new` looks
# for -- `<prefix>/bin/wolang` beside `<prefix>/lib/std` (D132).
if(PROJECT_IS_TOP_LEVEL)
    if(WOLANG_BUILD_TOOLS)
        install(TARGETS wolang_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    # The embedding library is exported as a target, so that a host can
    # `find_package(Wolang CONFIG)` and link `Wolang::embed` without
    # knowing where anything is (D137).
    # The core's headers come with it: exporting a target means a host
    # can link it, and linking it means its headers have to be there.
    install(TARGETS wolang_embed wolang_core wolang_runtime
        EXPORT WolangTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(TARGETS wolang_embed_shared
        EXPORT WolangTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT WolangTargets
        FILE WolangTargets.cmake
        NAMESPACE Wolang::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Wolang)
    # `Wolang::embed` rather than `Wolang::wolang_embed`: the namespace
    # already says whose it is.
    set_target_properties(wolang_embed PROPERTIES EXPORT_NAME embed)
    set_target_properties(wolang_embed_shared PROPERTIES
        EXPORT_NAME embed_shared)
    set_target_properties(wolang_core PROPERTIES EXPORT_NAME core)
    set_target_properties(wolang_runtime PROPERTIES EXPORT_NAME runtime)

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/WolangConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/WolangConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Wolang)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/WolangConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)
    install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/WolangConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/WolangConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Wolang)

    # And for a host that does not use CMake.
    configure_file(${PROJECT_SOURCE_DIR}/cmake/wolang-embed.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/wolang-embed.pc @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/wolang-embed.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
    install(DIRECTORY
            ${PROJECT_SOURCE_DIR}/lib/std
            ${PROJECT_SOURCE_DIR}/lib/http
            ${PROJECT_SOURCE_DIR}/lib/game
            ${PROJECT_SOURCE_DIR}/lib/core
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PATTERN "*.wo"
        PATTERN "wolang.toml")
    install(FILES
            ${PROJECT_SOURCE_DIR}/include/wolang/embed/wolang.h
            ${PROJECT_SOURCE_DIR}/include/wolang/embed/wolang.hpp
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
