#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

#check root rights
if [[ $EUID -ne 0 ]]; then
    echo "you must be root to run this script"
    exit 1
fi

apt-get update 
apt-get -y install build-essential
apt-get -y install build-essential cmake
apt-get -y install libelf-dev
apt-get -y install pkg-config
apt-get -y install clang
apt-get -y install m4
apt-get -y install bpftool
apt install -y libpcap-dev
apt install -y llvm
apt-get -y install linuxptp
apt-get -y install lldpd
