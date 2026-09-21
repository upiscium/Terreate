add_library(terreate_core STATIC
  src/core.cpp)

target_include_directories(terreate_core
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

terreate_configure_target(terreate_core)

set_target_properties(terreate_core PROPERTIES
  EXPORT_NAME Core)

# The alias is the same name in a source build and in the installed export.
add_library(Terreate::Core ALIAS terreate_core)

install(TARGETS terreate_core
  EXPORT TerreateTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp")
