#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

cleanup()
{
	exit 0
}

trap cleanup SIGINT SIGTERM

if [ "$TSN_APP_MODE" == "-l" ]; then

	# The exporter starts first and expects the logs directory to exist
	/usr/bin/mkdir logs

	# Start TSNApp Exporter in the background
	/usr/bin/python3 main.py --log-dir logs/ \
			--mode $TSN_APP_EXPORTER_MODE \
			--exporter-port $TSN_APP_EXPORTER_LISTEN_PORT \
			--log-level $TSN_APP_EXPORTER_LOG_LEVEL &

	sudo ./tsn_test_app -l $TSN_APP_LISTENER_INFINITY \
						$TSN_APP_LISTENER_LOGS \
						-u -d $TSN_APP_LISTENER_DUMMY_IP \
						-r $TSN_APP_TIMESTAMPING \
						$TSN_APP_LISTENER_QUIET

elif [ "$TSN_APP_MODE" == "-t" ]; then
	while true
	do
		sudo ./tsn_test_app -t -u \
						-p $TSN_APP_PROVISION_TIME \
						-o $TSN_APP_TRANSMISSION_OFFSET \
						-s $TSN_APP_TIMESTAMPING \
						-n $TSN_APP_TALKER_PACKETS \
						-d $TSN_APP_TALKER_DST_DNS \
						-L $TSN_APP_TALKER_PACKET_LENGTH \
						-c $TSN_APP_TALKER_CYCLE_TIME

		# Wait between to talker runs so that timeout expires in case of lost last packet
		/usr/bin/sleep $TSN_APP_PAUSE_TIME
	done

elif [ "$TSN_APP_MODE" == "-tN" ]; then
	while true
	do
	sudo ./tsn_test_app -t -u \
					-p $TSN_APP_PROVISION_TIME \
					-o $TSN_APP_TRANSMISSION_OFFSET \
					-s $TSN_APP_TIMESTAMPING \
					-N \
					-d $TSN_APP_TALKER_DST_DNS \
					-L $TSN_APP_TALKER_PACKET_LENGTH \
					-c $TSN_APP_TALKER_CYCLE_TIME
		# Wait between to talker runs so that timeout expires in case of lost last packet
		/usr/bin/sleep $TSN_APP_PAUSE_TIME
	done
else
	/usr/bin/echo "Error: TSN_APP_MODE environment variable wrong value!"

fi
