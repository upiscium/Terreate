if(NOT TARGET terreate_core)
  message(FATAL_ERROR
    "Terreate Platform requires Terreate Core; refusing to create an "
    "upward or implicit component dependency")
endif()

# GLFW is Platform's only native dependency. Graphics and Core never search
# for it, so consumers that do not request Platform need no GLFW (#285).
# 3.4 is the first release with the null backend and glfwGetPlatform.
find_package(glfw3 3.4 CONFIG REQUIRED)
if(NOT TARGET glfw)
  message(FATAL_ERROR "Terreate Platform requires the glfw target from glfw3 3.4")
endif()

add_library(terreate_platform STATIC
  src/runtime.cpp
  src/runtime_impl.hpp
  include/terreate/platform/runtime.hpp
  src/window.cpp
  include/terreate/platform/window.hpp)
target_include_directories(terreate_platform
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Public because the static archive's references to GLFW resolve in the
# consumer's link; the public headers themselves never include GLFW.
target_link_libraries(terreate_platform PUBLIC Terreate::Core glfw)
terreate_configure_target(terreate_platform)

set_target_properties(terreate_platform PROPERTIES
  EXPORT_NAME Platform)
add_library(Terreate::Platform ALIAS terreate_platform)

install(TARGETS terreate_platform
  EXPORT TerreatePlatformTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
