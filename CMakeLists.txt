cmake_minimum_required(VERSION 3.20)
project(TestCoroutine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

add_executable(test_binary TestCoroutine.cpp)

target_include_directories(test_binary PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(test_binary PRIVATE
            -Wall
            -Wextra
            -stdlib=libc++
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(test_binary PRIVATE
            -Wall
            -Wextra
    )
endif()

if(APPLE)
    set_target_properties(test_binary PROPERTIES
            MACOSX_RPATH ON
    )
endif()