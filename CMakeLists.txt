cmake_minimum_required (VERSION 3.25...4.0.0 FATAL_ERROR)
cmake_policy (SET CMP0155 NEW)

set (PROJECT_DESCRIPTION "limit order book lab" CACHE STRING "project description")
set_property (GLOBAL PROPERTY USE_FOLDERS ON)

# Default to Release, but never FORCE: Debug builds (ASan/UBSan + per-step
# invariant checks) must stay possible.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set (CMAKE_BUILD_TYPE Release CACHE STRING "build type")
endif ()

set (VCPKG_TARGET_TRIPLET x64-linux-clang-libcxx CACHE STRING "VCPKG target triplet")

project (book VERSION 0.1.0 LANGUAGES CXX C DESCRIPTION ${PROJECT_DESCRIPTION})

enable_testing ()

set (CMAKE_CXX_STANDARD 26)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_EXPORT_COMPILE_COMMANDS ON)

# valgrind 3.22 cannot decode AVX-512 (EVEX) instructions, so the `profile`
# preset overrides this with x86-64-v3. native artifacts must be rebuilt on the
# target machine.
set (LOB_MARCH native CACHE STRING "value passed to -march")

find_program (CCACHE_FOUND ccache)
if (CCACHE_FOUND)
    set (CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_FOUND})
endif ()

find_package (Catch2 CONFIG REQUIRED)
find_package (benchmark CONFIG REQUIRED)

# ---- shared compile/link options -------------------------------------------
add_library (lob_options INTERFACE)
target_compile_options (
    lob_options
    INTERFACE -Wall
              -Wextra
              -pedantic
              -Werror
              -stdlib=libc++
              -march=${LOB_MARCH}
              $<$<CONFIG:Debug>:-g
              -fno-omit-frame-pointer
              -fsanitize=address,undefined>
              # -g does not change codegen; perf annotate / cg_annotate need it
              $<$<CONFIG:Release>:-O3
              -g>
)
target_compile_definitions (
    lob_options INTERFACE $<$<CONFIG:Debug>:LOB_CHECK_INVARIANTS=1>
)
target_link_options (
    lob_options INTERFACE -stdlib=libc++ -lc++ -lc++abi
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# ---- core library: everything in source/ except main.cxx -------------------
file (GLOB_RECURSE __SOURCES__ CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cxx
)
set (__LIB_SOURCES__ ${__SOURCES__})
list (FILTER __LIB_SOURCES__ EXCLUDE REGEX ".*/main\\.cxx$")

add_library (lob_core STATIC ${__LIB_SOURCES__})
target_include_directories (lob_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries (lob_core PUBLIC lob_options)

# ---- CLI: gen / info / itch2ops / validate / replay / latency --------------
add_executable (${PROJECT_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/source/main.cxx)
target_link_libraries (${PROJECT_NAME} PRIVATE lob_core)

# ---- tests ------------------------------------------------------------------
file (GLOB_RECURSE __TEST_SOURCES__ CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/test/*.cxx
)
if (__TEST_SOURCES__)
    add_executable (test_${PROJECT_NAME} ${__TEST_SOURCES__})
    target_link_libraries (
        test_${PROJECT_NAME} PRIVATE lob_core Catch2::Catch2WithMain
    )
    # Catch2 macros use __COUNTER__
    target_compile_options (test_${PROJECT_NAME} PRIVATE -Wno-c2y-extensions)
    include (Catch)
    catch_discover_tests (test_${PROJECT_NAME} DISCOVERY_MODE PRE_TEST)
endif ()

# ---- benchmarks (not registered with ctest: run via scripts/bench.sh) -------
file (GLOB_RECURSE __BENCHMARK_SOURCES__ CONFIGURE_DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/*.cxx
)
if (__BENCHMARK_SOURCES__)
    add_executable (benchmark_${PROJECT_NAME} ${__BENCHMARK_SOURCES__})
    target_link_libraries (
        benchmark_${PROJECT_NAME} PRIVATE lob_core benchmark::benchmark
    )
endif ()
