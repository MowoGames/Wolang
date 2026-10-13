add_library(wolang_core STATIC
    base/arena.cc
    base/diagnostic.cc
    base/diagnostic_renderer.cc
    base/source_manager.cc
    base/string_interner.cc
    base/unicode.cc
    compiler/cache.cc
    compiler/database.cc
    compiler/item_tree.cc
    compiler/manifest.cc
    compiler/module.cc
    compiler/workspace.cc
    format/formatter.cc
    query/query_engine.cc
    interp/interpreter.cc
    interp/value.cc
    ir/builder.cc
    ir/ir.cc
    ir/lower.cc
    ir/passes.cc
    ir/printer.cc
    ir/verifier.cc
    sema/dimension.cc
    sema/effect.cc
    sema/resolution.cc
    sema/type.cc
    sema/type_check.cc
    syntax/ast.cc
    syntax/ast_printer.cc
    syntax/lexer.cc
    syntax/parser.cc
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
        ${PROJECT_SOURCE_DIR}/include/wolang/base/unicode.h
        ${PROJECT_SOURCE_DIR}/include/wolang/compiler/cache.h
        ${PROJECT_SOURCE_DIR}/include/wolang/compiler/database.h
        ${PROJECT_SOURCE_DIR}/include/wolang/compiler/item_tree.h
        ${PROJECT_SOURCE_DIR}/include/wolang/compiler/manifest.h
        ${PROJECT_SOURCE_DIR}/include/wolang/compiler/module.h
        ${PROJECT_SOURCE_DIR}/include/wolang/compiler/workspace.h
        ${PROJECT_SOURCE_DIR}/include/wolang/format/formatter.h
        ${PROJECT_SOURCE_DIR}/include/wolang/query/query_engine.h
        ${PROJECT_SOURCE_DIR}/include/wolang/interp/interpreter.h
        ${PROJECT_SOURCE_DIR}/include/wolang/interp/value.h
        ${PROJECT_SOURCE_DIR}/include/wolang/ir/builder.h
        ${PROJECT_SOURCE_DIR}/include/wolang/ir/ir.h
        ${PROJECT_SOURCE_DIR}/include/wolang/ir/lower.h
        ${PROJECT_SOURCE_DIR}/include/wolang/ir/passes.h
        ${PROJECT_SOURCE_DIR}/include/wolang/ir/printer.h
        ${PROJECT_SOURCE_DIR}/include/wolang/ir/verifier.h
        ${PROJECT_SOURCE_DIR}/include/wolang/ir/ir_opcode.inc
        ${PROJECT_SOURCE_DIR}/include/wolang/sema/dimension.h
        ${PROJECT_SOURCE_DIR}/include/wolang/sema/effect.h
        ${PROJECT_SOURCE_DIR}/include/wolang/sema/resolution.h
        ${PROJECT_SOURCE_DIR}/include/wolang/sema/type.h
        ${PROJECT_SOURCE_DIR}/include/wolang/sema/type_check.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/ast.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/ast_printer.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/lexer.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/parser.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/token.h
        ${PROJECT_SOURCE_DIR}/include/wolang/syntax/token_kind.h)

target_include_directories(wolang_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

wolang_configure_target(wolang_core)

add_library(wolang::core ALIAS wolang_core)
