# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(blackmagic)

include(components/frogfs/cmake/functions.cmake)
target_add_frogfs(blackmagic.elf html NAME frogfs CONFIG frogfs_config.json)

# Fill in variables inside `version.h.in` and generate `version.h`
cmake_minimum_required(VERSION 3.0.0)
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
configure_file(${CMAKE_SOURCE_DIR}/main/include/version.h.in ${CMAKE_SOURCE_DIR}/main/include/version.h @ONLY)

# ESP-IDF creates this as part of the link step, but doesn't have it listed
# in the BYPRODUCTS list. Add it here in order to silence the warning.
add_custom_target(create_mapfile
  COMMAND echo "Creating mapfile"
  BYPRODUCTS "blackmagic.map")

# Enable debug symbols, making it easier to work with the generated ELF file.
set(CMAKE_CXX_FLAGS_RELEASE  "${CMAKE_CXX_FLAGS_RELEASE} -g -ggdb3")
set(CMAKE_C_FLAGS_RELEASE  "${CMAKE_C_FLAGS_RELEASE} -g -ggdb3")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -ggdb3")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -ggdb3")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -g -ggdb3")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -g -ggdb3")
