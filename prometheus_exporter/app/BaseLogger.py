# SPDX-FileCopyrightText: Copyright 2025 Siemens AG
#
# SPDX-License-Identifier: Apache-2.0

"""
Creates new logger base object.
The Project's logger inherits from this one.
"""

import logging

class CustomFormatter(logging.Formatter):
    """
    Custom Formatter to format the output of the logger.
    Indludes: colors, timestamps, log level, class name - function name, message, file and line.
    """

    grey = "\x1b[38;20m"
    """Grey Color."""
    blue = "\x1b[38;5;39m"
    """Blue Color."""
    yellow = "\x1b[33;20m"
    """Yellow Color."""
    red = "\x1b[31;20m"
    """Red Color."""
    bold_red = "\x1b[31;1m"
    """Bold Red Color."""
    reset = "\x1b[0m"
    """Resets the color."""
    format = f"{reset} %(asctime)s - %(levelname)s {reset} - %(threadName)s - %(name)s - %(funcName)s  - %(message)s (%(filename)s:%(lineno)d)"
    """Format object. Includes timestamp, log level, calss name, etc. """

    FORMATS = {
        logging.DEBUG: f"%(asctime)s - {grey} %(levelname)s {reset} - %(threadName)s - %(name)s - %(funcName)s  - %(message)s (%(filename)s:%(lineno)d)",
        logging.INFO: f"%(asctime)s - {blue} %(levelname)s {reset} - %(threadName)s - %(name)s - %(funcName)s  - %(message)s (%(filename)s:%(lineno)d)",
        logging.WARNING: f"%(asctime)s - {yellow} %(levelname)s {reset} - %(threadName)s - %(name)s - %(funcName)s  - %(message)s (%(filename)s:%(lineno)d)",
        logging.ERROR: f"%(asctime)s - {red} %(levelname)s {reset} - %(threadName)s - %(name)s - %(funcName)s  - %(message)s (%(filename)s:%(lineno)d)",
        logging.CRITICAL: f"%(asctime)s - {bold_red} %(levelname)s {reset} - %(threadName)s - %(name)s - %(funcName)s  - %(message)s (%(filename)s:%(lineno)d)",
        }

    def format(self, record: logging.LogRecord) -> str:
        """
        Format the logger.
        """
        log_fmt = self.FORMATS.get(record.levelno)
        formatter = logging.Formatter(log_fmt)
        return formatter.format(record)

logging.basicConfig(level=logging.DEBUG)
logger = logging.getLogger('main')
# To avoid duplicate logs.
logger.propagate = False

# Create console handler with a higher log level
ch = logging.StreamHandler()
# Log level done through argparse in main
#ch.setLevel(logging.DEBUG)
#ch.setLevel(logging.INFO)

ch.setFormatter(CustomFormatter())

logger.addHandler(ch)

