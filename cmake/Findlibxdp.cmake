# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

#
# Try to find an installed libxdp.
#
# We search in ${CMAKE_PREFIX_PATH} and as fallback in ${CMAKE_INSTALL_PREFIX}.
# So if this file reports libxdp as "not found", please make sure
# your build parameters (especially CMAKE_PREFIX_PATH) are correctly set.
#

if (USE_LIBXDP_FROM_SUBMODULE)
    message(
            STATUS
            "libxdp will be build from sources"
    )
    return()
endif ()

# Search for the interface (headers)
find_path(LIBXDP_INCLUDE_DIR
        NAMES xdp/libxdp.h
        HINTS ${CMAKE_PREFIX_PATH}/include ${CMAKE_INSTALL_PREFIX}/include)

if (LIBXDP_INCLUDE_DIR)
    message(
            STATUS
            "libxdp interface found at: ${LIBXDP_INCLUDE_DIR}"
    )
else ()
    message(
            WARNING
            "libxdp interface not found! Forgot to set CMAKE_PREFIX_PATH?"
    )
endif ()

# Search for the libxdp library
find_library(LIBXDP_LIBRARY NAMES xdp
        HINTS ${CMAKE_PREFIX_PATH}/lib ${CMAKE_INSTALL_PREFIX}/lib)

if (LIBXDP_LIBRARY)
    message(
            STATUS
            "libxdp found at ${LIBXDP_LIBRARY}"
    )
else ()
    message(
            WARNING
            "libxdp not found! Forgot to set CMAKE_PREFIX_PATH?"
    )
endif ()
