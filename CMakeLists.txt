cmake_minimum_required(VERSION 3.20)
project(TestCoroutine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O2")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(./include)

add_executable(test_binary TestCoroutine.cpp)

if(APPLE)
    target_compile_options(test_binary PRIVATE -stdlib=libc++)
endif()