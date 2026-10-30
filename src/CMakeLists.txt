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
# Position-independent, because `libwolang_embed.so` is built out of it.
set_target_properties(wolang_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
    # Sockets, and the cryptographic provider a token is made from.
    target_link_libraries(wolang_runtime PUBLIC ws2_32 bcrypt secur32)

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
    target_link_libraries(wolang_core PUBLIC ws2_32 bcrypt secur32)

endif()

# The ahead-of-time backend, when LLVM is available. Through the C API
# rather than the C++ one: this project builds without RTTI and LLVM's
# C++ headers want it (D98).
if(WOLANG_WITH_LLVM)
    target_compile_definitions(wolang_core PUBLIC WOLANG_WITH_LLVM=1)
    target_include_directories(wolang_core SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
endif()
if(WOLANG_LLVM_C)
    # One library that is all of LLVM, every target included.
    target_include_directories(wolang_core SYSTEM PRIVATE "${WOLANG_LLVM_C_ROOT}/include")
    target_link_libraries(wolang_core PUBLIC "${WOLANG_LLVM_C_ROOT}/lib/LLVM-C.lib")
    target_compile_definitions(wolang_core PUBLIC WOLANG_ALL_TARGETS=1)
elseif(WOLANG_WITH_LLVM)
    # Every target this LLVM has, not just this machine's: a compiler
    # that can only emit code for the machine it runs on is one that
    # cannot build the Windows binary from the laptop, and `--target` is
    # what makes that possible (D160).
    option(WOLANG_ALL_TARGETS "Emit code for every target LLVM has" ON)
    if(WOLANG_ALL_TARGETS)
        # Named one by one rather than as `all-targets`: that group is a
        # `llvm-config` spelling, and the CMake function wants the
        # components -- which are whatever this LLVM was built with.
        llvm_map_components_to_libnames(WOLANG_LLVM_LIBS
            core analysis passes bitwriter
            ${LLVM_TARGETS_TO_BUILD})
        target_compile_definitions(wolang_core PUBLIC
            WOLANG_ALL_TARGETS=1)
    else()
        llvm_map_components_to_libnames(WOLANG_LLVM_LIBS
            core target native analysis passes bitwriter)
    endif()
    target_link_libraries(wolang_core PUBLIC ${WOLANG_LLVM_LIBS})
endif()

# Where the runtime archive ends up, so that `wolang build` can hand it
# to the linker without being told.
target_compile_definitions(wolang_core PRIVATE
    # The compiler's version has one home, project(VERSION) (D248).
    WOLANG_VERSION_STRING="${PROJECT_VERSION}"
    WOLANG_RUNTIME_ARCHIVE="$<TARGET_FILE:wolang_runtime>"
    # The C runtime the archive was built against, which a binary linked
    # with it on Windows has to use as well.
    WOLANG_RUNTIME_CRT="$<IF:$<CONFIG:Debug>,/MDd,/MD>"
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
target_compile_definitions(wolang_embed PRIVATE
    WOLANG_VERSION_STRING="${PROJECT_VERSION}")
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
# A shared library on Windows brings an import library with it, and that
# one is also called `wolang_embed.lib` -- the same name the static
# library has, which is two rules writing one file. The import library
# gets its own name; everywhere else the two already differ by their
# extension (D187).
if(WIN32)
    set_target_properties(wolang_embed_shared PROPERTIES
        ARCHIVE_OUTPUT_NAME wolang_embed_dll
        # And a DLL exports nothing unless it is asked to, which for a
        # project that marks nothing is every symbol.
        WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
target_link_libraries(wolang_embed_shared PRIVATE wolang_core)
target_compile_definitions(wolang_embed_shared PRIVATE
    WOLANG_VERSION_STRING="${PROJECT_VERSION}")
target_include_directories(wolang_embed_shared PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
wolang_configure_target(wolang_embed_shared)
