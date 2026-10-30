# Test sources are found rather than listed, the same way the library's are.
file(GLOB_RECURSE WOLANG_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/harness/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc")

add_executable(wolang_tests ${WOLANG_TEST_SOURCES})

# The embedding header must stay C: one C11 translation unit includes it
# and asserts the layout of a WolangValue (D241).
enable_language(C)
add_library(wolang_abi_c_check OBJECT unit/embed/abi_c_check.c)
set_target_properties(wolang_abi_c_check PROPERTIES C_STANDARD 11
    C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(wolang_abi_c_check PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
target_sources(wolang_tests PRIVATE $<TARGET_OBJECTS:wolang_abi_c_check>)

target_include_directories(wolang_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# `platform/*.h` decoders are header-only so they can be tested here (D247).
target_include_directories(wolang_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                           ${PROJECT_SOURCE_DIR})
# Lets the whole-file tests find the bundled examples.
target_compile_definitions(wolang_tests PRIVATE
    WOLANG_PROJECT_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(wolang_tests PRIVATE wolang::core)
wolang_ship_llvm_c(wolang_tests)
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
