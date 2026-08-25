#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

# Start the first process
ptp4l -i ens19 -2 -A -S -m &
  
# Start the second process
./tsn_test_app -A -i ens19 -s AL &
  
# Wait for any process to exit
wait -n
  
# Exit with status of process that exited first
exit $?
