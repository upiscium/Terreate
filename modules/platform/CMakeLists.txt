if(NOT TARGET terreate_core)
  message(FATAL_ERROR
    "Terreate Platform requires Terreate Core; refusing to create an "
    "upward or implicit component dependency")
endif()

add_library(terreate_platform STATIC src/platform.cpp)
target_include_directories(terreate_platform
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(terreate_platform PUBLIC Terreate::Core)
terreate_configure_target(terreate_platform)

set_target_properties(terreate_platform PROPERTIES
  EXPORT_NAME Platform)
add_library(Terreate::Platform ALIAS terreate_platform)

install(TARGETS terreate_platform
  EXPORT TerreatePlatformTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
