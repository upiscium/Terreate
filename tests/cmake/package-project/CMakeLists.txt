cmake_minimum_required(VERSION 3.28)
project(TerreatePackageFixture LANGUAGES CXX)

if(NOT DEFINED TERREATE_SOURCE_DIR OR "${TERREATE_SOURCE_DIR}" STREQUAL "")
  message(FATAL_ERROR "TERREATE_SOURCE_DIR is required")
endif()

add_subdirectory("${TERREATE_SOURCE_DIR}" terreate)
