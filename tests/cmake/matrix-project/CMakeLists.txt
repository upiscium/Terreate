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
    get_target_property(_interface_includes terreate_${_target_lower}
      INTERFACE_INCLUDE_DIRECTORIES)
    if("${_interface_includes}" MATCHES "(^|;)${_target_name}/src(;|$)" OR
       "${_interface_includes}" MATCHES "/src")
      message(FATAL_ERROR
        "private ${_target_name} include directory leaked into its public "
        "interface: ${_interface_includes}")
    endif()
    get_target_property(_module_scanning terreate_${_target_lower}
      CXX_SCAN_FOR_MODULES)
    if(NOT "${_module_scanning}" STREQUAL "OFF")
      message(FATAL_ERROR
        "C++ module scanning must be OFF for Terreate::${_target_name}")
    endif()
  endif()
endforeach()

if(TARGET terreate_core)
  get_target_property(_core_links terreate_core LINK_LIBRARIES)
  if("${_core_links}" MATCHES "Platform|Graphics|platform|graphics")
    message(FATAL_ERROR "Core has an upward component dependency: ${_core_links}")
  endif()
endif()
if(TARGET terreate_graphics)
  get_target_property(_graphics_links terreate_graphics LINK_LIBRARIES)
  if("${_graphics_links}" MATCHES "platform|Platform")
    message(FATAL_ERROR
      "Graphics must not depend on Platform: ${_graphics_links}")
  endif()
endif()

if(_component STREQUAL "Core")
  set(_consumer_source [=[
#include <project/lib.hpp>
int main() { return answer() == 42 ? 0 : 1; }
]=])
elseif(_component STREQUAL "Platform")
  set(_consumer_source [=[
#include <terreate/platform.hpp>
int main() { terreate::platform::component_anchor(); return 0; }
]=])
elseif(_component STREQUAL "Graphics")
  set(_consumer_source [=[
#include <terreate/graphics.hpp>
int main() { terreate::graphics::component_anchor(); return 0; }
]=])
else()
  message(FATAL_ERROR "unknown matrix component ${_component}")
endif()

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
