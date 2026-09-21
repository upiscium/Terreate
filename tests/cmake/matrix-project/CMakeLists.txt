cmake_minimum_required(VERSION 3.28)
project(TerreateComponentMatrix LANGUAGES CXX)

foreach(_required_variable IN ITEMS TERREATE_SOURCE_DIR TERREATE_MATRIX_COMPONENT)
  if(NOT DEFINED ${_required_variable} OR
     "${${_required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${_required_variable} is required")
  endif()
endforeach()

add_subdirectory("${TERREATE_SOURCE_DIR}" terreate)

set(_component "${TERREATE_MATRIX_COMPONENT}")
if(NOT TARGET Terreate::${_component})
  message(FATAL_ERROR "expected Terreate::${_component} to be configured")
endif()

foreach(_component_name IN ITEMS Core Platform Graphics)
  string(TOUPPER "${_component_name}" _component_upper)
  if(TERREATE_EXPECT_${_component_upper})
    if(NOT TARGET Terreate::${_component_name})
      message(FATAL_ERROR
        "expected enabled target Terreate::${_component_name}")
    endif()
  elseif(TARGET Terreate::${_component_name})
    message(FATAL_ERROR
      "disabled component unexpectedly created Terreate::${_component_name}")
  endif()
endforeach()

foreach(_target_name IN ITEMS Core Platform Graphics)
  string(TOLOWER "${_target_name}" _target_lower)
  if(TARGET terreate_${_target_lower})
    foreach(_interface_property IN ITEMS
        INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
      get_target_property(_interface_includes terreate_${_target_lower}
        ${_interface_property})
      if("${_interface_includes}" MATCHES "(^|;)[^;]*/src(;|$)" OR
         "${_interface_includes}" MATCHES "/src/")
        message(FATAL_ERROR
          "private ${_target_name} include directory leaked into its public "
          "${_interface_property}: ${_interface_includes}")
      endif()
    endforeach()
    get_target_property(_module_scanning terreate_${_target_lower}
      CXX_SCAN_FOR_MODULES)
    if(NOT "${_module_scanning}" STREQUAL "OFF")
      message(FATAL_ERROR
        "C++ module scanning must be OFF for Terreate::${_target_name}")
    endif()
  endif()
endforeach()

foreach(_link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
  if(TARGET terreate_core)
    get_target_property(_core_links terreate_core ${_link_property})
    if("${_core_links}" MATCHES "[Pp]latform|[Gg]raphics")
      message(FATAL_ERROR
        "Core has an upward component dependency in ${_link_property}: "
        "${_core_links}")
    endif()
  endif()
  if(TARGET terreate_platform)
    get_target_property(_platform_links terreate_platform ${_link_property})
    if(NOT "${_platform_links}" MATCHES "[Cc]ore")
      message(FATAL_ERROR
        "Platform must retain its public Core dependency in "
        "${_link_property}: ${_platform_links}")
    endif()
    if("${_platform_links}" MATCHES "[Gg]raphics")
      message(FATAL_ERROR
        "Platform has an invalid Graphics dependency in ${_link_property}: "
        "${_platform_links}")
    endif()
  endif()
  if(TARGET terreate_graphics)
    get_target_property(_graphics_links terreate_graphics ${_link_property})
    if(NOT "${_graphics_links}" MATCHES "[Cc]ore")
      message(FATAL_ERROR
        "Graphics must retain its public Core dependency in "
        "${_link_property}: ${_graphics_links}")
    endif()
    if("${_graphics_links}" MATCHES "[Pp]latform")
      message(FATAL_ERROR
        "Graphics must not depend on Platform in ${_link_property}: "
        "${_graphics_links}")
    endif()
  endif()
endforeach()

if(NOT _component MATCHES "^(Core|Platform|Graphics)$")
  message(FATAL_ERROR "unknown matrix component ${_component}")
endif()

set(_consumer_source [=[
int main() { return 0; }
]=])

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/matrix_consumer.cpp"
  "${_consumer_source}")
add_executable(matrix_consumer
  "${CMAKE_CURRENT_BINARY_DIR}/matrix_consumer.cpp")
target_link_libraries(matrix_consumer PRIVATE Terreate::${_component})
set_target_properties(matrix_consumer PROPERTIES
  CXX_STANDARD 23
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
set_property(TARGET matrix_consumer PROPERTY CXX_SCAN_FOR_MODULES OFF)
