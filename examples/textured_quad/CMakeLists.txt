# A gap-finding example: a textured, indexed, depth-tested quad pair with a
# per-frame uniform buffer, written with Terreate's API (see main.cpp for the native
# calls it still makes). Built with the Platform-Graphics Integration
# (TERREATE_BUILD_EXAMPLES); not installed.
add_executable(terreate_example_textured_quad main.cpp quad_spirv.hpp)
target_link_libraries(terreate_example_textured_quad PRIVATE Terreate::IntegrationPlatformGraphics)
target_include_directories(terreate_example_textured_quad PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
terreate_configure_target(terreate_example_textured_quad)

# The scripted run, with the same conventions as example.triangle: 90 frames, a
# resize at frame 30 that must happen, the validation layer required present and
# silent; SKIP, never PASS, without a window system or when the window system does
# not resize the window.
add_test(NAME example.textured_quad
  COMMAND terreate_example_textured_quad --frames 90 --resize-at 30 --require-validation
          --require-resize)
set_tests_properties(example.textured_quad PROPERTIES
  PASS_REGULAR_EXPRESSION "example\\.textured_quad: frames=PASS"
  SKIP_REGULAR_EXPRESSION "example\\.textured_quad: (frames|resize)=SKIP"
  FAIL_REGULAR_EXPRESSION "example\\.textured_quad: error"
  TIMEOUT 60)
