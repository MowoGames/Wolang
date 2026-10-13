add_executable(wolang_tests
    harness/test.cc
    harness/test_main.cc
    unit/base/arena_unittest.cc
    unit/base/diagnostic_unittest.cc
    unit/base/source_manager_unittest.cc
    unit/base/string_interner_unittest.cc
    unit/syntax/lexer_unittest.cc
    unit/syntax/parser_unittest.cc
    unit/syntax/token_kind_unittest.cc)

target_include_directories(wolang_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Lets the whole-file tests find the bundled examples.
target_compile_definitions(wolang_tests PRIVATE
    WOLANG_PROJECT_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(wolang_tests PRIVATE wolang::core)
wolang_configure_target(wolang_tests)

add_test(NAME unit COMMAND wolang_tests)

# Keep the suites individually addressable so a failure points at a component.
foreach(suite Arena StringInterner SourceManager SourceSpanTest Diagnostic
              TokenKind Lexer Parser)
  add_test(NAME unit.${suite} COMMAND wolang_tests --filter ${suite})
endforeach()
