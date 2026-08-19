#!/usr/bin/env python3
"""Minimal SIMCOM AT probe (tty). Usage: tools/at_probe.py [PORT] CMD [CMD...]"""
from __future__ import annotations

import os
import select
import sys
import termios
import time

BAUD = termios.B115200


def open_at(path: str) -> int:
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0
    a[1] = 0
    a[3] = a[3] & ~(termios.ECHO | termios.ICANON | termios.ISIG | termios.IEXTEN)
    a[2] = a[2] | (termios.CLOCAL | termios.CREAD)
    a[4] = BAUD
    a[5] = BAUD
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def drain(fd: int) -> None:
    while True:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            break
        try:
            os.read(fd, 4096)
        except BlockingIOError:
            break


def at(fd: int, cmd: str, timeout: float = 3.0) -> str:
    drain(fd)
    os.write(fd, (cmd + "\r\n").encode())
    termios.tcdrain(fd)
    out = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if not r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            chunk = b""
        if not chunk:
            continue
        out += chunk
        s = out.decode("utf-8", "replace")
        if "\nOK" in s or s.strip().endswith("OK") or "ERROR" in s or "NOT IN" in s:
            time.sleep(0.05)
            while True:
                r2, _, _ = select.select([fd], [], [], 0.02)
                if not r2:
                    break
                try:
                    out += os.read(fd, 4096)
                except BlockingIOError:
                    break
            break
    return out.decode("utf-8", "replace")


def main() -> int:
    args = sys.argv[1:]
    port = "/dev/ttyUSB2"
    if args and args[0].startswith("/dev/"):
        port = args.pop(0)
    if not args:
        args = ["AT", "AT+CPSI?", "AT+CNMP?", "AT+CLECELL?", "AT+CCELLCFG?", "AT+CLUCELL?"]
    fd = open_at(port)
    try:
        at(fd, "ATE0", 1)
        for cmd in args:
            t = 120.0 if "COPS=?" in cmd else 8.0 if "CFUN" in cmd or "COPS=" in cmd else 3.0
            print(f">>> {cmd}")
            print(at(fd, cmd, t).strip() or "(empty)")
            print()
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
