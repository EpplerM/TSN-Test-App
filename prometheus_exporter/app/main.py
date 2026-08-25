# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import logging
import BaseLogger
import traceback

import signal
from TSNTestAppExporter import TSNTestAppExporter
from types import FrameType
from watchdog.observers import Observer
from time import sleep

def handler(signum: int, frame: FrameType) -> None:
    """
    Properly close the program when ctrl-c.
    """
    logger.warning("Received ctrl-c! The program will exit soon. Be patient!")
    observer.stop()
    observer.join()
    exit(1)

if __name__ == "__main__":

    parser = argparse.ArgumentParser()
    parser.add_argument("--log-level", default="INFO", choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"], help="Log level (default: %(default)s).")
    parser.add_argument("--log-dir", default="./logs", help="Log dir location, relative to script (default: %(default)s).")
    parser.add_argument("--delete-logs", default="y", type=str, choices=["y", "n"], help="Should the exporter delete old log files? (default: %(default)s).")
    parser.add_argument("--mode", default="native", help="Where is the TSN Test App running? (default: %(default)s).")
    parser.add_argument("--exporter-port", default=9101, type=int, help="The port that the exporter listens to (default: %(default)s).")
    parser.add_argument("--exporter-ip", default="0.0.0.0", help="The ip that the exporter listens to (default: %(default)s).")

    args = parser.parse_args()

    logging.getLogger("watchdog").setLevel(logging.WARNING)

    logger = logging.getLogger('main')
    logger.setLevel(f"{args.log_level}")
    signal.signal(signal.SIGINT, handler)

    tsn_app_exporter = TSNTestAppExporter(log_dir = args.log_dir,
                                          delete = args.delete_logs,
                                          mode = args.mode,
                                          exporter_port = args.exporter_port,
                                          exporter_ip = args.exporter_ip)
    observer = Observer()
    observer.schedule(tsn_app_exporter, args.log_dir, recursive=False)
    observer.start()
    try:
        while True:
            sleep(0.1)
    finally:
        observer.stop()
        observer.join()

