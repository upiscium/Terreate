# The minimal getting-started program (headless). Built with TERREATE_BUILD_EXAMPLES; not
# installed. docs/getting-started.md includes this source, and the installed-package
# smoke builds the same file as an external consumer, so neither can drift from it.
add_executable(terreate_example_getting_started main.cpp)
target_link_libraries(terreate_example_getting_started PRIVATE Terreate::Graphics)
terreate_configure_target(terreate_example_getting_started)

add_test(NAME example.getting_started COMMAND terreate_example_getting_started)
set_tests_properties(example.getting_started PROPERTIES
  PASS_REGULAR_EXPRESSION "example\\.getting_started: bootstrap=PASS"
  FAIL_REGULAR_EXPRESSION "example\\.getting_started: bootstrap=BLOCKED")
