# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

#
# Try to find an installed libbpf.
#
# We search in ${CMAKE_PREFIX_PATH} and as fallback in ${CMAKE_INSTALL_PREFIX}.
# So if this file reports libbpf as "not found", please make sure
# your build parameters (especially CMAKE_PREFIX_PATH) are correctly set.
#

if (USE_LIBXDP_FROM_SUBMODULE)
    message(
            STATUS
            "libbpf will be build from sources"
    )
    return()
endif ()

# Search for the interface (headers)
find_path(LIBBPF_INCLUDE_DIR
        NAMES bpf/bpf.h
        HINTS ${CMAKE_PREFIX_PATH}/include ${CMAKE_INSTALL_PREFIX}/include)

if (LIBBPF_INCLUDE_DIR)
    message(
            STATUS
            "libbpf interface found at: ${LIBBPF_INCLUDE_DIR}"
    )
else ()
    message(
            WARNING
            "libbpf interface not found! Forgot to set CMAKE_PREFIX_PATH?"
    )
endif ()

# Search for the libbpf library
find_library(LIBBPF_LIBRARY NAMES bpf
        HINTS ${CMAKE_PREFIX_PATH}/lib ${CMAKE_INSTALL_PREFIX}/lib)

if (LIBBPF_LIBRARY)
    message(
            STATUS
            "libbpf found at ${LIBBPF_LIBRARY}"
    )
else ()
    message(
            WARNING
            "libbpf not found! Forgot to set CMAKE_PREFIX_PATH?"
    )
endif ()
