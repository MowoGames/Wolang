add_executable(wolang_cli main.cc)
set_target_properties(wolang_cli PROPERTIES OUTPUT_NAME wolang)
target_link_libraries(wolang_cli PRIVATE wolang::core)
wolang_configure_target(wolang_cli)
