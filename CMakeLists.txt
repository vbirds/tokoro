cmake_minimum_required(VERSION 3.20)
project(TestCoroutine)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译器优化级别
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

# 如果没有指定构建类型，默认使用 Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 添加头文件目录
include_directories(./include)

# 创建可执行文件
add_executable(test_binary TestCoroutine.cpp)

# 针对 macOS 的特殊设置（如果需要）
if(APPLE)
    target_compile_options(test_binary PRIVATE -stdlib=libc++)
endif()