cmake_minimum_required(VERSION 3.10)
project(vdplus LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(vdplus SHARED native.cpp)
target_compile_options(vdplus PRIVATE -std=c++17 -fPIC -O2)
target_link_libraries(vdplus log dl)
