# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.1.0)
project(es_to_es)

# Build options
# Can be set using the cmake cmdline during build configuration like
# cmake -DENABLE_XY=ON
option(ENABLE_PROVISIONTIME "" ON)
option(ENABLE_DEBUG_RECV "" OFF)
option(ENABLE_DEBUG_SEND "" OFF)
option(ENABLE_DEBUG_TS "" OFF)
option(ENABLE_CLASSIC_SLEEP "" ON)
option(USE_LIBXDP_FROM_SUBMODULE "" OFF)

if (ENABLE_PROVISIONTIME)
    add_compile_definitions(ENABLE_PROVISIONTIME)
endif()

if (ENABLE_DEBUG_RECV)
    add_compile_definitions(ENABLE_DEBUG_RECV)
endif()

if (ENABLE_DEBUG_SEND)
    add_compile_definitions(ENABLE_DEBUG_SEND)
endif()

if (ENABLE_DEBUG_TS)
    add_compile_definitions(ENABLE_DEBUG_TS)
endif()

if (ENABLE_CLASSIC_SLEEP)
    add_compile_definitions(ENABLE_CLASSIC_SLEEP)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/)

# Configure compiler settings
if (${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu11 -Wall -fPIC")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -s -O2 -DNDEBUG")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g3 -ggdb -fprofile-arcs -ftest-coverage")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} --coverage")
elseif (${CMAKE_C_COMPILER_ID} STREQUAL "Intel")
elseif (${CMAKE_C_COMPILER_ID} STREQUAL "MSVC")
elseif (${CMAKE_C_COMPILER_ID} STREQUAL "MINGW")
endif ()

add_subdirectory("src")
