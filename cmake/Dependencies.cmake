# Third-party dependencies: Boost and {fmt}.
#
# Both are pinned archives fetched through FetchDep.cmake into the shared
# dependency cache, then built as static libraries from their own CMake
# projects. They keep their own language settings: the C++26 requirement is
# applied to this project's own targets (ToolchainChecks.cmake), and the public
# headers are compiled by C++26 consumers, which is what proves the
# compatibility.

include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/FetchDep.cmake)

# --- Boost 1.86.0 ----------------------------------------------------------->
#
# The official CMake archive (the `boost-1.86.0-cmake.tar.xz` release asset) is
# a superproject of per-library CMake projects, which is what allows building
# only the libraries this project links.
set(LUA_DEBUG_BOOST_VERSION "1.86.0")
fetch_dep(
    NAME boost
    URL "https://github.com/boostorg/boost/releases/download/boost-${LUA_DEBUG_BOOST_VERSION}/boost-${LUA_DEBUG_BOOST_VERSION}-cmake.tar.xz"
    SHA256 "2c5ec5edcdff47ff55e27ed9560b0a0b94b07bd07ed9928b476150e16b0efc57"
    EXTRACT_DIR "boost-${LUA_DEBUG_BOOST_VERSION}"
    MARKERS "CMakeLists.txt" "libs/process/CMakeLists.txt" "libs/asio/CMakeLists.txt")
set(LUA_DEBUG_BOOST_SOURCE_DIR "${LUA_DEBUG_DEP_CACHE}/boost-${LUA_DEBUG_BOOST_VERSION}")

# --- {fmt} 12.1.0 ----------------------------------------------------------->
#
# fmt is used instead of <format>/<print>: the shipped binaries still run on
# macOS 10.15, whose libc++ has neither.
set(LUA_DEBUG_FMT_VERSION "12.1.0")
fetch_dep(
    NAME fmt
    URL "https://github.com/fmtlib/fmt/releases/download/${LUA_DEBUG_FMT_VERSION}/fmt-${LUA_DEBUG_FMT_VERSION}.zip"
    SHA256 "695fd197fa5aff8fc67b5f2bbc110490a875cdf7a41686ac8512fb480fa8ada7"
    EXTRACT_DIR "fmt-${LUA_DEBUG_FMT_VERSION}"
    MARKERS "CMakeLists.txt" "include/fmt/format.h")
set(LUA_DEBUG_FMT_SOURCE_DIR "${LUA_DEBUG_DEP_CACHE}/fmt-${LUA_DEBUG_FMT_VERSION}")

# Static libraries only: the built binaries must not require the target machine
# to carry Boost or fmt shared objects, and a debuggee process loads a bridge.
set(BUILD_SHARED_LIBS OFF)

# Everything ends up inside a DSO on some path (the runtime library, the
# bridges, the launcher), so third-party objects are position independent
# regardless of the consumer. Boost's own targets inherit this at creation time.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The Frida devkits are built against the static release CRT, so every object
# linked next to them uses /MT, including debug builds; /MTd would fail on
# _ITERATOR_DEBUG_LEVEL. Boost would otherwise pick /MTd for Debug.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Boost.Asio, Boost.Process v2, Boost.Unordered: the platform layer that replaces
# bee. The superproject resolves the transitive closure
# (algorithm, config, core, fusion, iterator, move, optional, system, tokenizer,
# type_index, winapi, ...) from the `Boost::<lib>` references in each library's
# CMakeLists.
set(BOOST_INCLUDE_LIBRARIES
    "asio;process;unordered;system"
    CACHE STRING "Boost libraries built by this project")
set(BOOST_EXCLUDE_LIBRARIES "")
set(BOOST_SKIP_INSTALL_RULES ON)
set(BOOST_RUNTIME_LINK static)
set(BUILD_TESTING OFF)

add_subdirectory("${LUA_DEBUG_BOOST_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/boost" EXCLUDE_FROM_ALL)

# The compiled Process v2 library has to agree with our translation units on the
# path type (`std::filesystem::path`, see common/path_utf8.h): the symbol
# signatures carry it. PUBLIC, so every target linking it inherits the switch.
if(TARGET boost_process)
    target_compile_definitions(boost_process PUBLIC BOOST_PROCESS_USE_STD_FS)
    # Boost declares C++11 as its minimum and the compiler default here is
    # gnu++14, where libc++ has no <filesystem> at all.
    set_target_properties(boost_process PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
endif()

# Boost builds with hidden visibility by default; keep the static archives
# usable from our DSOs without leaking their symbols into the publish tree.

# --- {fmt} -----------------------------------------------------------------
set(FMT_INSTALL OFF)
set(FMT_TEST OFF)
set(FMT_DOC OFF)
set(FMT_PEDANTIC OFF)
set(FMT_WERROR OFF)
add_subdirectory("${LUA_DEBUG_FMT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/fmt" EXCLUDE_FROM_ALL)

if(NOT TARGET Boost::asio OR NOT TARGET Boost::process OR NOT TARGET Boost::system)
    message(FATAL_ERROR "lua-debug: Boost targets are missing after add_subdirectory (expected Boost::asio, Boost::process, Boost::system)")
endif()
if(NOT TARGET fmt::fmt)
    message(FATAL_ERROR "lua-debug: fmt::fmt is missing after add_subdirectory")
endif()
