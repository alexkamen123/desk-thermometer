#!/usr/bin/env python3
"""Read ESP32 USB logs across deep sleep without setting DTR/RTS (macOS/POSIX).

pyserial is used only for port enumeration. Serial.open toggles modem lines on
this board and can cause USB_UART_CHIP_RESET, destroying wake-cause evidence.
"""
import argparse
import fcntl
import os
import select
import termios
import time
import tty
from pathlib import Path
from serial.tools import list_ports

parser = argparse.ArgumentParser()
parser.add_argument('--serial', required=True)
parser.add_argument('--seconds', type=int, default=720)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
deadline = time.monotonic() + args.seconds
fd = None
buffer = b''
with args.output.open('a', buffering=1) as log:
    def record(message):
        line = time.strftime('%Y-%m-%d %H:%M:%S') + ' ' + message
        log.write(line + '\n')
        print(line, flush=True)
    record('CAPTURE START raw POSIX, no modem-line ioctls')
    try:
        while time.monotonic() < deadline:
            try:
                if fd is None:
                    candidates = [p for p in list_ports.comports() if (p.serial_number or '').lower().replace(':', '') == args.serial.lower().replace(':', '')]
                    if not candidates:
                        time.sleep(0.25)
                        continue
                    fd = os.open(candidates[0].device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                    fcntl.ioctl(fd, termios.TIOCEXCL)
                    tty.setraw(fd, termios.TCSANOW)
                    attrs = termios.tcgetattr(fd)
                    attrs[2] = (attrs[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
                    attrs[4] = attrs[5] = termios.B115200
                    termios.tcsetattr(fd, termios.TCSANOW, attrs)
                    buffer = b''
                    record('CONNECTED ' + candidates[0].device)
                ready, _, _ = select.select([fd], [], [], 0.3)
                if not ready:
                    continue
                data = os.read(fd, 4096)
                if not data:
                    raise OSError('USB closed')
                buffer += data
                while b'\n' in buffer:
                    line, buffer = buffer.split(b'\n', 1)
                    record(line.decode('utf-8', errors='replace').rstrip())
            except OSError as error:
                record('DISCONNECTED ' + str(error))
                if fd is not None:
                    os.close(fd)
                fd = None
                time.sleep(0.5)
    finally:
        if fd is not None:
            os.close(fd)
    record('CAPTURE COMPLETE')
