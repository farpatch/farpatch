# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(farpatch)

include(components/frogfs/cmake/functions.cmake)
target_add_frogfs(farpatch.elf html NAME frogfs CONFIG frogfs_config.json)

# Fill in variables inside `version.h.in` and generate `version.h`
message(STATUS "Resolving GIT Version")
set(_build_version "unknown")
find_package(Git)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _farpatch_build_version
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C components/blackmagic/blackmagic describe --always --dirty --tags
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _bmp_version
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  message( STATUS "Farpatch version: ${_farpatch_build_version}")
  message( STATUS "Blackmagic version: ${_bmp_version}")
else()
  message(STATUS "GIT not found")
endif()
string(TIMESTAMP _time_stamp)

configure_file(
  ${CMAKE_SOURCE_DIR}/main/include/version.h.in
  ${CMAKE_SOURCE_DIR}/main/include/version.h
  @ONLY
)

# Regenerate version.h on every build to ensure it's not stale
add_custom_command(
  OUTPUT ${CMAKE_SOURCE_DIR}/main/include/version.h.in
  COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_SOURCE_DIR}/main/include/version.h.in
  DEPENDS ${CMAKE_SOURCE_DIR}/main/include/version.h.in
  COMMENT "Regenerating version.h"
)

# ESP-IDF creates this as part of the link step, but doesn't have it listed
# in the BYPRODUCTS list. Add it here in order to silence the warning.
add_custom_target(create_mapfile
  COMMAND echo "Creating mapfile"
  BYPRODUCTS "farpatch.map")

# Add a define that turns `network_changed` into `network_changed_link_down`.
# This works around esp-idf issue #14582, and should be removed once that is closed.
idf_build_set_property(COMPILE_DEFINITIONS "dhcp_network_changed=dhcp_network_changed_link_up" APPEND)
