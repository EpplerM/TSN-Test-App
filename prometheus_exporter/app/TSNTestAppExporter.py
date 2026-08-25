# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

from prometheus_client import start_http_server, Gauge

import netifaces

import logging
import BaseLogger
import os
import traceback

from watchdog.events import FileSystemEventHandler
from watchdog.events import FileCreatedEvent

class TSNTestAppExporter(FileSystemEventHandler):
    """
    Representation of Prometheus Metrics and loop to fetch
    """

    def __init__(self, log_dir: str="./logs", delete: str="y", mode: str="native", exporter_port: int=9101, exporter_ip: str="0.0.0.0") -> None:
        self.logger = logging.getLogger(f"main.{__class__.__name__}")
        self.logger.info("Initializing TSN Test App Exporter")
        self.logger.info(f"Listening at {exporter_ip}:{exporter_port}")

        # Arguments
        self.log_dir = log_dir
        self.delete = delete
        self.mode = mode

        vlan_id = ""
        interfaces = netifaces.interfaces()

        for i in range(0, len(interfaces)):
            if "net." in interfaces[i]:
                vlan_id = interfaces[i].split(".")[-1]

        self.labels = (mode, vlan_id)

        # Metrics
        self.min_delay = Gauge(f"tsn_test_app_min_delay", f"Minimum delay of last test.", labelnames=["mode", "vlan_id"])
        self.min_delay.labels(*self.labels)

        self.max_delay = Gauge(f"tsn_test_app_max_delay", f"Maximum delay of last test.", labelnames=["mode", "vlan_id"])
        self.max_delay.labels(*self.labels)

        self.avg_delay = Gauge(f"tsn_test_app_avg_delay", f"Average delay of last test.", labelnames=["mode", "vlan_id"])
        self.avg_delay.labels(*self.labels)

        self.jitter = Gauge(f"tsn_test_app_jitter", f"Jitter of last test.", labelnames=["mode", "vlan_id"])
        self.jitter.labels(*self.labels)

        self.packet_loss = Gauge(f"tsn_test_app_packet_loss", f"Lost packets of last test.", labelnames=["mode", "vlan_id"])
        self.packet_loss.labels(*self.labels)


        try:
            # Runs as a daemon thread.
            start_http_server(int(exporter_port), addr=exporter_ip)
        except OSError as e:
            self.logger.error(f"{e}\n{traceback.format_exc()}")

    def on_created(self, event: FileCreatedEvent) -> None:
        """
        Called when new file in log directory is created.
        """
        self.logger.info(f"New file detected in { self.log_dir }!")
        self.fetch()

    def fetch(self) -> None:
        """
        Actually fetch the metrics from the TSN Test App and refresh Prometheus' metrics with new values.
        """

        # Fetch Data
        logs = os.listdir(self.log_dir)
        logs = [i for i in logs if "logfile" in i and not "PROCESSED" in i]
        logs.sort()

        # Delete corrupted log files in case logging was interrupted by a error in the Listener.
        # Delete oldest log files, if there are more than 2 log files.
        if self.delete == "y" and len(logs) > 2:
            self.logger.info(f"Deleting {len(logs)-2} corrupt log file(s)")
            for i in range(0, len(logs)-2):
                os.remove(f"{self.log_dir}/{logs[i]}")
            # Fetch Data again
            logs = os.listdir(self.log_dir)
            logs = [i for i in logs if "logfile" in i and not "PROCESSED" in i]
            logs.sort()

        if not logs:
            self.logger.warning("No new log files to process!")
            return

        f = open(self.log_dir+"/"+logs[0])
        self.logger.info(f"Proccesing: { f.name }")

        lines = f.readlines()

        f.close()

        if not lines or "Latency" not in lines[-1]:
            self.logger.warning("Test run not finished yet!")
            return

        if self.delete == "y":
            # Delete old log file
            os.remove(f.name)
        else:
            # Rename file name, so that you don't process it later again.
            print("Renaming")
            os.rename(f.name, f"{self.log_dir}/PROCESSED_{f.name.split('/')[-1]}")

        metrics = lines[-1].split(" ")

        avg_delay = metrics[0].split(":")[-1].lstrip("\\t").replace(",", "")
        self.avg_delay.labels(*self.labels).set(avg_delay)
        self.logger.debug(f"AVG DELAY: {avg_delay}")

        min_delay = metrics[3].lstrip("\\t").replace(",", "")
        self.min_delay.labels(*self.labels).set(min_delay)
        self.logger.debug(f"MIN DELAY: {min_delay}")

        max_delay = metrics[6].lstrip("\\t").replace(",", "")
        self.max_delay.labels(*self.labels).set(max_delay)
        self.logger.debug(f"MAX DELAY: {max_delay}")

        jitter = metrics[10].lstrip("\\t").replace(",", "")
        self.jitter.labels(*self.labels).set(jitter)
        self.logger.debug(f"JITTER: {jitter}")

        metrics = lines[-3].split(" ")

        packet_loss = str((float(metrics[3]) - float(metrics[1])) / float(metrics[3])) 
        self.packet_loss.labels(*self.labels).set(packet_loss)
        self.logger.debug(f"PACKET LOSS: {packet_loss}")

        # Free up memory
        lines = 0

