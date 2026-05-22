#!/usr/bin/env python3
"""Read an ESP32-S3 USB serial port without toggling DTR/RTS reset lines."""

from __future__ import annotations

import argparse
import os
import sys
import termios
import time


def open_raw_serial(path: str, baud: int) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    speed = getattr(termios, f"B{baud}")
    attrs[4] = speed
    attrs[5] = speed
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=None, help="serial port, defaults to first /dev/cu.usbmodem*")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=0.0, help="stop after N seconds; 0 means run until Ctrl-C")
    parser.add_argument("--send", default=None, help="line to write after opening, without adding reset toggles")
    args = parser.parse_args()

    port = args.port
    if not port:
        import glob

        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if not ports:
            print("no /dev/cu.usbmodem* port found", file=sys.stderr)
            return 1
        port = ports[0]

    fd = open_raw_serial(port, args.baud)
    deadline = time.time() + args.seconds if args.seconds > 0 else None
    if args.send is not None:
        os.write(fd, (args.send.rstrip("\r\n") + "\n").encode())

    try:
        while deadline is None or time.time() < deadline:
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                chunk = b""
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
