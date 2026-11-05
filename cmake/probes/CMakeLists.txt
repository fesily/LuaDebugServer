# Configure-time probe project.
#
# try_compile() runs this project with the parent's compiler, standard and
# platform flags. Only the third-party include directories are passed in,
# through a generated file: Boost exposes one include directory per library and
# a semicolon-separated list cannot be handed over on a -D flag unchanged.
cmake_minimum_required(VERSION 4.2...4.4)

project(lua_debug_cxx26_probe LANGUAGES CXX)

if(NOT PROBE_INCLUDES_FILE OR NOT EXISTS "${PROBE_INCLUDES_FILE}")
    message(FATAL_ERROR "PROBE_INCLUDES_FILE is missing: the parent build did not publish the Boost/fmt include directories")
endif()
include("${PROBE_INCLUDES_FILE}")

# A static library on purpose: at configure time Boost has not been built yet, so
# only compilation is checked here. The link (and the run) is covered by the
# luadebug_toolchain_probe target, which is part of the normal build.
add_library(probe STATIC cxx26_probe.cpp)
set_target_properties(probe PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)
target_include_directories(probe PRIVATE ${PROBE_INCLUDE_DIRS})
