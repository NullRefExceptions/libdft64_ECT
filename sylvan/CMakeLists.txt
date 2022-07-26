cmake_minimum_required(VERSION 3.13)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/")

project(Sylvan
    VERSION 1.6.1
    DESCRIPTION "Sylvan, a parallel decision diagram library"
    HOMEPAGE_URL "https://github.com/trolando/sylvan"
    LANGUAGES C CXX
)


# Print some info
message(STATUS "CMake build configuration for Sylvan ${PROJECT_VERSION}")
message(STATUS "OS: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")


# Include a few useful CMake functionality
include(CheckSymbolExists)
include(CMakeDependentOption)


# By default, build as RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
ENDIF(NOT CMAKE_BUILD_TYPE)


# Set up the target library
add_library(Sylvan)
add_library(Sylvan::Sylvan ALIAS Sylvan)
set_target_properties(Sylvan PROPERTIES VERSION ${Sylvan_VERSION} SOVERSION ${Sylvan_VERSION_MAJOR})
add_subdirectory(src)





