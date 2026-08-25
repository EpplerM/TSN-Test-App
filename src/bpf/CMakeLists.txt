# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

set(BPF_CFLAGS "BPF_CFLAGS=${BPF_CFLAGS} ${BPF_INCLUDE_DIRS}")

add_custom_target(
        bpf-program
        COMMAND CC=${CMAKE_C_COMPILER} ${BPF_CFLAGS} make
        COMMAND mv *.ll *.o ${CMAKE_CURRENT_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
add_dependencies(tsn_test_app bpf-program)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/eth_type_filter.o
        DESTINATION share/e2e)
