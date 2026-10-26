add_executable(wolang_cli main.cc)
set_target_properties(wolang_cli PROPERTIES OUTPUT_NAME wolang)
target_link_libraries(wolang_cli PRIVATE wolang::core)
wolang_configure_target(wolang_cli)
wolang_ship_llvm_c(wolang_cli)

# Where the standard library is in this tree, so the driver's own commands --
# `new`, above all -- can name the standard library (D132).
target_compile_definitions(wolang_cli PRIVATE
    WOLANG_LIBRARY_DIRECTORY="${PROJECT_SOURCE_DIR}/lib")
