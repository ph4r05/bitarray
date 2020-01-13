cmake_minimum_required(VERSION 3.6)
project(bitarray)

include_directories(PYTHON_INCLUDE_DIRS
        "/opt/local/Library/Frameworks/Python.framework/Versions/2.7/include/python2.7")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(SOURCE_FILES
    bitarray/_bitarray.c)

add_executable(bitarray ${SOURCE_FILES})