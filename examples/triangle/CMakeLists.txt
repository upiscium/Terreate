# A windowed triangle written with Terreate's API only. Built with the
# Platform-Graphics Integration (TERREATE_BUILD_EXAMPLES); not installed.
add_executable(terreate_example_triangle main.cpp triangle_spirv.hpp)
target_link_libraries(terreate_example_triangle PRIVATE Terreate::IntegrationPlatformGraphics)
target_include_directories(terreate_example_triangle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
terreate_configure_target(terreate_example_triangle)

# The scripted run: 90 frames, a resize at frame 30, and the validation layer
# required to be present and silent. It reports SKIP, never PASS, without a
# window system.
add_test(NAME example.triangle
  COMMAND terreate_example_triangle --frames 90 --resize-at 30 --require-validation)
set_tests_properties(example.triangle PROPERTIES
  PASS_REGULAR_EXPRESSION "example\\.triangle: frames=PASS"
  SKIP_REGULAR_EXPRESSION "example\\.triangle: frames=SKIP"
  FAIL_REGULAR_EXPRESSION "example\\.triangle: error")
