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

include(cmake/WolangCompilerOptions.cmake)

add_subdirectory(src)

if(WOLANG_BUILD_TOOLS)
    add_subdirectory(tools/wolang)
endif()

if(WOLANG_BUILD_TESTS AND PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
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
