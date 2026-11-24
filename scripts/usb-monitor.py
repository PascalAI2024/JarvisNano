#!/usr/bin/env python3
"""Read an ESP32-S3 USB serial port without toggling DTR/RTS reset lines.

Works on both POSIX (raw termios) and Windows (pyserial with DTR/RTS held low
and automatic ESP32-S3 VID 0x303A port discovery).
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
import time
from typing import Any

ESPRESSIF_VID = 0x303A


def detect_port_pyserial() -> str | None:
    """Auto-detect an ESP32-S3 USB JTAG/Serial port (VID 0x303A) via pyserial."""
    from serial.tools import list_ports

    ports = list(list_ports.comports())
    esp_matches = [p for p in ports if getattr(p, "vid", None) == ESPRESSIF_VID]
    if esp_matches:
        esp_matches.sort(key=lambda p: (
            0 if any(k in (p.description or "").lower() for k in ("jtag", "serial", "usb")) else 1,
            p.device,
        ))
        return esp_matches[0].device
    if len(ports) == 1:
        return ports[0].device
    return None


def detect_port_posix() -> str | None:
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def open_raw_serial(path: str, baud: int) -> int:
    """Open serial device on POSIX via raw termios without toggling modem lines."""
    import termios

    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    speed = getattr(termios, f"B{baud}")
    attrs[4] = speed
    attrs[5] = speed
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def open_pyserial(path: str, baud: int) -> Any:
    """Open serial port via pyserial without pulsing DTR/RTS reset lines."""
    import serial

    ser = serial.Serial(
        port=None,
        baudrate=baud,
        timeout=0.05,
        dsrdtr=False,
        rtscts=False,
    )
    ser.dtr = False
    ser.rts = False
    ser.port = path
    ser.open()
    try:
        ser.dtr = False
        ser.rts = False
    except OSError:
        pass
    return ser


def _run_pyserial(port: str | None, baud: int, seconds: float, send: str | None) -> int:
    try:
        import serial  # noqa: F401
        from serial.tools import list_ports
    except ImportError:
        print(
            "usb-monitor.py requires pyserial on this platform: pip install pyserial",
            file=sys.stderr,
        )
        return 2

    if not port:
        port = detect_port_pyserial()
        if not port:
            avail = [
                f"{p.device} ({p.description or 'unknown'}, vid={hex(p.vid) if p.vid else 'n/a'})"
                for p in list_ports.comports()
            ]
            if avail:
                print(
                    "no ESP32-S3 (VID 0x303A) port found; pass --port. Available:\n  "
                    + "\n  ".join(avail),
                    file=sys.stderr,
                )
            else:
                print("no serial ports found", file=sys.stderr)
            return 1

    ser = open_pyserial(port, baud)
    deadline = time.time() + seconds if seconds > 0 else None
    if send is not None:
        ser.write((send.rstrip("\r\n") + "\n").encode("utf-8"))
        ser.flush()

    try:
        while deadline is None or time.time() < deadline:
            waiting = getattr(ser, "in_waiting", 0) or 1
            chunk = ser.read(min(4096, waiting))
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return 0


def _run_posix(port: str | None, baud: int, seconds: float, send: str | None) -> int:
    if not port:
        port = detect_port_posix()
        if not port:
            print("no /dev/cu.usbmodem* or /dev/ttyACM* port found", file=sys.stderr)
            return 1

    fd = open_raw_serial(port, baud)
    deadline = time.time() + seconds if seconds > 0 else None
    if send is not None:
        os.write(fd, (send.rstrip("\r\n") + "\n").encode("utf-8"))

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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port",
        default=None,
        help="serial port (auto-detects ESP32-S3 VID 0x303A on Windows or /dev/cu.usbmodem* on POSIX)",
    )
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--seconds",
        type=float,
        default=0.0,
        help="stop after N seconds; 0 means run until Ctrl-C",
    )
    parser.add_argument(
        "--send",
        default=None,
        help="line to write after opening, without adding reset toggles",
    )
    args = parser.parse_args(argv)

    use_posix = False
    if sys.platform != "win32":
        try:
            import termios  # noqa: F401
            use_posix = True
        except ImportError:
            use_posix = False

    if use_posix:
        return _run_posix(args.port, args.baud, args.seconds, args.send)
    return _run_pyserial(args.port, args.baud, args.seconds, args.send)


if __name__ == "__main__":
    raise SystemExit(main())
