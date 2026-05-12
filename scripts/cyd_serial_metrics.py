#!/usr/bin/env python3
"""Read CYD USB serial (115200) and print parsed METRIC / STATS / INA226 boot lines.

Requires: pip install pyserial

Typical CYD (CH340) on Linux: /dev/ttyUSB*
  ESPPORT=/dev/ttyUSB6 ./scripts/cyd_serial_metrics.py
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial", file=sys.stderr)
    raise SystemExit(2) from None

RE_METRIC_OK = re.compile(
    r"^METRIC vbus_v=(?P<v>[\d.+-]+) i_a=(?P<i>[\d.+-]+) p_w=(?P<p>[\d.+-]+)"
)
RE_METRIC_ERR = re.compile(r"^METRIC sensor_ok=0 err=(?P<e>.+)")
RE_STATS = re.compile(r"^STATS cpu=(?P<c>\d+) ram=(?P<r>\d+)")
RE_INA226_BOOT = re.compile(
    r"^INA226 mfg_id=0x(?P<m>[0-9A-Fa-f]{4}) die_id=0x(?P<d>[0-9A-Fa-f]{4})"
)
RE_INA226_RAW_PREFIX = "INA226_RAW "


def guess_ch340_ports() -> list[str]:
    """Best-effort: list ttyUSB nodes that look like QinHeng CH340."""
    import glob

    import subprocess

    out: list[str] = []
    for path in sorted(glob.glob("/dev/ttyUSB*")):
        try:
            p = subprocess.run(
                ["udevadm", "info", "-q", "property", "-n", path],
                capture_output=True,
                text=True,
                timeout=2,
                check=False,
            )
        except OSError:
            continue
        if "ID_VENDOR_ID=1a86" in p.stdout and "ID_MODEL_ID=7523" in p.stdout:
            out.append(path)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description="Monitor CYD serial for METRIC / STATS lines.")
    ap.add_argument(
        "--port",
        "-p",
        default=os.environ.get("ESPPORT") or os.environ.get("CYD_SERIAL") or "",
        help="Serial device (default: $ESPPORT or $CYD_SERIAL)",
    )
    ap.add_argument(
        "--seconds",
        type=float,
        default=30.0,
        help="How long to read (0 = run until Ctrl+C)",
    )
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    port = args.port.strip()
    if not port:
        candidates = guess_ch340_ports()
        if len(candidates) == 1:
            port = candidates[0]
            print(f"# Using guessed port {port} (CH340)", file=sys.stderr)
        else:
            print(
                "Set --port or ESPPORT= e.g. /dev/ttyUSB6\n"
                f"CH340 candidates: {candidates or '(none found)'}",
                file=sys.stderr,
            )
            raise SystemExit(1)

    ser = serial.Serial(port, args.baud, timeout=0.25)
    print(f"# Opened {port} @ {args.baud}", file=sys.stderr)

    buf = b""
    t_end = None if args.seconds <= 0 else time.monotonic() + args.seconds
    n_metric_ok = n_metric_err = 0

    try:
        while True:
            if t_end is not None and time.monotonic() >= t_end:
                break
            chunk = ser.read(4096)
            if not chunk:
                continue
            buf += chunk
            while b"\r\n" in buf or b"\n" in buf:
                if b"\r\n" in buf:
                    line, buf = buf.split(b"\r\n", 1)
                else:
                    line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").strip()

                mo = RE_INA226_BOOT.match(text)
                if mo:
                    print(text, flush=True)
                    continue
                if text.startswith(RE_INA226_RAW_PREFIX):
                    print(text, flush=True)
                    continue
                mo = RE_STATS.match(text)
                if mo:
                    print(text, flush=True)
                    continue
                mo_ok = RE_METRIC_OK.match(text)
                if mo_ok:
                    n_metric_ok += 1
                    v = float(mo_ok.group("v"))
                    i_a = float(mo_ok.group("i"))
                    p_w = float(mo_ok.group("p"))
                    vi = v * i_a
                    delta = abs(p_w - vi) if abs(vi) > 1e-6 else 0.0
                    flag = ""
                    if v > 0.05 and i_a > 0.05:
                        pct = (100.0 * delta / vi) if abs(vi) > 1e-9 else 0.0
                        if pct > 15.0:
                            flag = f" | WARN large |P−V·I|: {pct:.1f}% of V·I"
                    print(f"{text}{flag}", flush=True)
                    continue
                mo_er = RE_METRIC_ERR.match(text)
                if mo_er:
                    n_metric_err += 1
                    print(text, flush=True)
                    continue
    except KeyboardInterrupt:
        print("# interrupted", file=sys.stderr)
    finally:
        ser.close()

    print(
        f"# Done. METRIC ok={n_metric_ok} err_lines={n_metric_err}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
