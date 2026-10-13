add_library(wolang_core STATIC
    base/arena.cc
    base/diagnostic.cc
    base/diagnostic_renderer.cc
    base/source_manager.cc
    base/string_interner.cc
    syntax/lexer.cc
    syntax/token.cc
    syntax/token_kind.cc)

target_sources(wolang_core PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}/include
    FILES
        ${PROJECT_SOURCE_DIR}/include/wolang/base/arena.h
        ${PROJECT_SOURCE_DIR}/include/wolang/base/check.h
        ${PROJECT_SOURCE_DIR}/include/wolang/base/diagnostic.h
        ${PROJECT_SOURCE_DIR}/include/wolang/base/diagnostic_renderer.h
        ${PROJECT_SOURCE_DIR}/include/wolang/base/source_manager.h
        ${PROJECT_SOURCE_DIR}/include/wolang/base/string_interner.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/lexer.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/token.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/token_kind.h)

target_include_directories(wolang_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

wolang_configure_target(wolang_core)

add_library(wolang::core ALIAS wolang_core)
