# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.1.0)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(libbpf REQUIRED)
find_package(libxdp REQUIRED)

set(E2E_BPF_PROGRAM_PATH "src/bpf/eth_type_filter.o" CACHE STRING "")
set(E2E_BPF_PROGRAM_NAME "xdp_sock_prog" CACHE STRING "")
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/e2e_build_cfg.h.in
        ${CMAKE_CURRENT_SOURCE_DIR}/e2e_build_cfg.h
)

set(
        SRC
        main.c e2e_agent.c e2e_cmdline.c e2e_clock.c e2e_thread.c
        e2e_listener.c e2e_talker.c e2e_config.c e2e_sock.c e2e_bpf.c
)
set_source_files_properties(${SRC} PROPERTIES LANGUAGE C)
add_executable(tsn_test_app ${SRC})

if (USE_LIBXDP_FROM_SUBMODULE)
    string(CONCAT LIBBPF_ARGS "CC=${CMAKE_C_COMPILER} "
                              "DESTDIR=${CMAKE_CURRENT_BINARY_DIR} "
                              "BUILD_STATIC_ONLY=1 "
                              "OBJDIR=${CMAKE_CURRENT_BINARY_DIR}/libbpf "
                              "LIBDIR=/usr/lib "
                              "make install")

    separate_arguments(LIBBPF_BUILD_CMD UNIX_COMMAND ${LIBBPF_ARGS})
    add_custom_target(
            libbpf
            COMMAND ${LIBBPF_BUILD_CMD}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libbpf/src
    )

    string(CONCAT LIBXDP_CFLAGS "-DHAVE_LIBBPF_BTF__LOAD_FROM_KERNEL_BY_ID "
                                "-DHAVE_LIBBPF_BTF__TYPE_CNT "
                                "-DHAVE_LIBBPF_BPF_OBJECT__NEXT_MAP "
                                "-DHAVE_LIBBPF_BPF_OBJECT__NEXT_PROGRAM "
                                "-DHAVE_LIBBPF_BPF_PROGRAM__INSN_CNT "
                                "-DHAVE_LIBBPF_BPF_XDP_ATTACH "
                                "-DHAVE_LIBBPF_BPF_MAP_CREATE "
                                "-DHAVE_SECURE_GETENV")

    string(CONCAT LIBXDP_ARGS "CC=${CMAKE_C_COMPILER} "
                              "LD=${CMAKE_LINKER} "
                              "OBJCOPY=${CMAKE_OBJCOPY} "
                              "DESTDIR=${CMAKE_CURRENT_BINARY_DIR} "
                              "LIBBPF_DIR=${CMAKE_CURRENT_SOURCE_DIR}/libbpf "
                              "LIBBPF_INCLUDE_DIR=${CMAKE_CURRENT_BINARY_DIR}/usr/include "
                              "LIBBPF_LIB_DIR=${CMAKE_CURRENT_BINARY_DIR}/usr/lib "
                              "CFLAGS=\"${LIBXDP_CFLAGS}\" "
                              "BPF_CFLAGS=\"${BPF_CFLAGS}\" "
                              "BUILD_STATIC_ONLY=1 "
                              "OBJDIR=${CMAKE_CURRENT_BINARY_DIR}/libxdp "
                              "PREFIX=/usr "
                              "make libxdp_install")

    separate_arguments(LIBXDP_BUILD_CMD UNIX_COMMAND ${LIBXDP_ARGS})
    add_custom_target(
            libxdp
            COMMAND ${LIBXDP_BUILD_CMD}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libxdp
    )
    add_dependencies(libxdp libbpf)
    add_dependencies(tsn_test_app libxdp)

    set(LIBXDP_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/usr/include)
    set(LIBXDP_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/usr/lib/libxdp.a)
    set(LIBBPF_LIBRARY ${CMAKE_CURRENT_BINARY_DIR}/usr/lib/libbpf.a)
    set(LIBXDP_DEPENDENCY_LIBS elf z)
    set(BPF_INCLUDE_DIRS -I${LIBXDP_INCLUDE_DIR})

    target_include_directories(tsn_test_app PRIVATE ${LIBXDP_INCLUDE_DIR})
endif ()

target_link_libraries(tsn_test_app
        PRIVATE
        Threads::Threads
        ${LIBXDP_LIBRARY}
        ${LIBBPF_LIBRARY}
        ${LIBXDP_DEPENDENCY_LIBS}
        )

install(TARGETS tsn_test_app RUNTIME DESTINATION sbin)

add_subdirectory(bpf)

if (USE_LIBXDP_FROM_SUBMODULE)
    add_dependencies(bpf-program libxdp)
endif ()
