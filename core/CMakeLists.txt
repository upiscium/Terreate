add_library(terreate_core STATIC
  src/core.cpp
  "${PROJECT_SOURCE_DIR}/src/lib.cpp")

target_include_directories(terreate_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

terreate_configure_target(terreate_core)

set_target_properties(terreate_core PROPERTIES
  EXPORT_NAME Core)

# The alias is the same name in a source build and in the installed export.
add_library(Terreate::Core ALIAS terreate_core)

install(TARGETS terreate_core
  EXPORT TerreateTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${PROJECT_SOURCE_DIR}/include/project/lib.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/project")
