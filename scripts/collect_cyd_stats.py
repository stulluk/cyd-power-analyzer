#!/usr/bin/env python3
"""Collect STATS lines from CYD UART and print averages (needs: pip install pyserial)."""

from __future__ import annotations

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)

STATS_RE = re.compile(r"STATS\s+cpu=(\d+)\s+ram=(\d+)")


def main() -> None:
    ap = argparse.ArgumentParser(description="Average CYD STATS lines over a time window.")
    ap.add_argument("--port", default="/dev/serial-cyd", help="Serial device")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=10.0, help="Collection window after warmup")
    ap.add_argument("--warmup", type=float, default=2.0, help="Discard time after open")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"Serial open failed: {e}", file=sys.stderr)
        sys.exit(2)

    try:
        ser.reset_input_buffer()
    except serial.SerialException:
        pass

    time.sleep(args.warmup)
    cpus: list[int] = []
    rams: list[int] = []
    t_end = time.monotonic() + args.seconds

    while time.monotonic() < t_end:
        try:
            raw = ser.readline()
        except serial.SerialException:
            time.sleep(0.05)
            continue
        line = raw.decode("utf-8", errors="ignore")
        m = STATS_RE.search(line)
        if not m:
            continue
        cpus.append(int(m.group(1)))
        rams.append(int(m.group(2)))

    ser.close()

    if not cpus:
        print("No STATS lines captured (check firmware, baud, port).", file=sys.stderr)
        sys.exit(3)

    n = len(cpus)
    print(f"samples={n} window_s={args.seconds:.1f} warmup_s={args.warmup:.1f}")
    print(f"cpu_avg={sum(cpus) / n:.1f}")
    print(f"ram_heap_used_pct_avg={sum(rams) / n:.1f}")


if __name__ == "__main__":
    main()
