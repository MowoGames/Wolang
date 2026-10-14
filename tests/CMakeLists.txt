# Test sources are found rather than listed, the same way the library's are.
file(GLOB_RECURSE WOLANG_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/harness/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")

add_executable(wolang_tests ${WOLANG_TEST_SOURCES})

target_include_directories(wolang_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Lets the whole-file tests find the bundled examples.
target_compile_definitions(wolang_tests PRIVATE
    WOLANG_PROJECT_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(wolang_tests PRIVATE wolang::core)
wolang_configure_target(wolang_tests)

add_test(NAME unit COMMAND wolang_tests)

# Keep the suites individually addressable so a failure points at a
# component. The names are read out of the sources: a `WOLANG_TEST(Suite,
# Case)` is the only place a suite name is written.
set(WOLANG_SUITES "")
foreach(source ${WOLANG_TEST_SOURCES})
  file(STRINGS ${source} matches REGEX "WOLANG_TEST\\(([A-Za-z_][A-Za-z_0-9]*),")
  foreach(match ${matches})
    string(REGEX REPLACE ".*WOLANG_TEST\\(([A-Za-z_][A-Za-z_0-9]*),.*" "\\1"
           suite "${match}")
    list(APPEND WOLANG_SUITES ${suite})
  endforeach()
endforeach()
list(REMOVE_DUPLICATES WOLANG_SUITES)
list(SORT WOLANG_SUITES)

foreach(suite ${WOLANG_SUITES})
  add_test(NAME unit.${suite} COMMAND wolang_tests --filter ${suite})
endforeach()
