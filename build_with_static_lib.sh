#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

if [ $1 == 'd12' ]
then
  ### for debian12
  rm -r build
  mkdir build
  cd build
  cmake .. -DBPF_CFLAGS=-I/usr/include/x86_64-linux-gnu
  cmake --build .
  echo "for new debian12 platform - start the app from /build directory using sudo ./src/tsn_test_app ... "
else
  rm -r build
  git submodule init
  git submodule update
  mkdir build
  cd build
  cmake .. -DUSE_LIBXDP_FROM_SUBMODULE=ON -DBPF_CFLAGS=-I/usr/include/x86_64-linux-gnu
  cmake --build .
  echo "start the app from /build directory using sudo ./src/tsn_test_app ... "
fi

date
