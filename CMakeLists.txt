cmake_minimum_required(VERSION 3.14)
project(fiskta VERSION 0.1.0 LANGUAGES C)

include(cmake/ClangTidy.cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(FISKTA_CLANG_TIDY "Build with Clang Tidy" OFF)
option(FISKTA_LTO "Build with Link-Time Optimization" ON)
option(FISKTA_SANITIZERS "Build with address, undefined behaviour and leak sanitizers" OFF)
option(FISKTA_TSAN "Build with thread sanitizer" OFF)

add_library(fiskta_build_options INTERFACE)
target_compile_options(fiskta_build_options INTERFACE
    -pedantic
    -Werror
    -W
    -Wall
    -Wextra
    -Wcast-align
    -Wcast-qual
    -Wmissing-declarations
    -Wwrite-strings
    -Wundef
    -Wswitch-enum
    -Wstrict-aliasing=2
    -Wno-switch-enum
    -Wno-deprecated-declarations
    $<$<C_COMPILER_ID:GNU>:
        -Wshadow
    >
    $<$<C_COMPILER_ID:Clang>:
        -Wshadow-all
    >
    $<$<BOOL:${FISKTA_SANITIZERS}>:
        -fsanitize=address,undefined,leak
    >
    $<$<BOOL:${FISKTA_TSAN}>:
        -fsanitize=thread
        $<$<C_COMPILER_ID:GNU>:
            -Wno-error=tsan
        >
    >
    $<$<BOOL:${FISKTA_LTO}>:
        $<$<C_COMPILER_ID:GNU>:
            -fipa-pta
        >
    >)
target_link_options(fiskta_build_options INTERFACE
    $<$<BOOL:${FISKTA_SANITIZERS}>:-fsanitize=address,undefined,leak>
    $<$<BOOL:${FISKTA_TSAN}>:-fsanitize=thread>)


file(GLOB_RECURSE INC "src/*.h")
file(GLOB_RECURSE SRC "src/*.c")

add_executable(fiskta)
target_sources(fiskta PRIVATE ${SRC} ${INC})
target_link_libraries(fiskta PRIVATE fiskta_build_options)

if (FISKTA_CLANG_TIDY)
    clang_tidy_check(fiskta)
endif()

if (FISKTA_LTO)
    cmake_policy(SET CMP0069 NEW)
    set_property(TARGET fiskta PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
