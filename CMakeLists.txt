cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb3 -O0 -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG -D_GLIBCXX_DEBUG -D_LIBCPP_ENABLE_DEBUG_MODE")
set(CMAKE_CXX_FLAGS_PROFILER "-O2 -pg -Wall -Werror -Wno-sign-compare -Wno-unused-function -Wno-unused-variable -DLTSM_DEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -Wall -Wno-sign-compare -Wno-unused-function -Wno-unused-variable")

option(LTSM_BUILD_PROFILING "build with profiling" OFF)
option(LTSM_BUILD_SANITIZE "build with sanitize" OFF)
option(LTSM_BUILD_CLIENT "build client" ON)
option(LTSM_BUILD_SERVER "build server" OFF)
option(LTSM_BUILD_TOOLS "build tools" OFF)
option(LTSM_BUILD_TESTS "build tests" OFF)

if(LTSM_BUILD_SANITIZE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fsanitize=float-divide-by-zero -fsanitize=float-cast-overflow -fno-sanitize=null -fno-sanitize=alignment")
endif()

if(LTSM_BUILD_PROFILING)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
endif()

if(LTSM_BUILD_COVERAGE_TESTS)
    set(CMAKE_CXX_FLAGS " ${CMAKE_CXX_FLAGS} --coverage -DLTSM_COVERAGE_TEST")
endif()

include(FindPkgConfig)
set(CMAKE_FIND_FRAMEWORK LAST)

project(LTSM)

if(APPLE)
    add_definitions(-D__APPLE__)
elseif(UNIX)
    add_definitions(-D__UNIX__)
elseif(MINGW)
    add_definitions(-D__MINGW__)
endif()

add_subdirectory(src)
