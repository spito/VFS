cmake_minimum_required(VERSION 2.8.5)
project(VFS)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ggdb3 -std=c++11 -DVFS_STANDALONE")

add_executable(fs _glue.cpp fs.cpp fs-dummyEntrypoint.cpp fs-manager.cpp fs-memory.cpp main.cpp)
