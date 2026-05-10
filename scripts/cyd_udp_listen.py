#!/usr/bin/env python3
"""Listen for CYD UDP telemetry JSON datagrams and append them to a text file."""

from __future__ import annotations

import argparse
import socket
import sys
import time
from datetime import datetime, timezone


def main() -> None:
    ap = argparse.ArgumentParser(description="Listen for CYD UDP telemetry (JSON lines).")
    ap.add_argument("--host", default="", help="Bind address (default all interfaces)")
    ap.add_argument("--port", type=int, default=4210, help="UDP port (must match CYD_UDP_TELEM_PORT)")
    ap.add_argument("--seconds", type=float, default=10.0, help="Listen duration")
    ap.add_argument("--output", "-o", required=True, help="Output text file path")
    ap.add_argument(
        "--no-echo",
        action="store_true",
        help="Do not print each packet to stdout (file only)",
    )
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind((args.host, args.port))
    except OSError as e:
        print(f"bind failed: {e}", file=sys.stderr)
        sys.exit(2)

    sock.settimeout(0.25)
    deadline = time.monotonic() + args.seconds
    count = 0

    with open(args.output, "a", encoding="utf-8") as out:
        out.write(f"# cyd_udp_listen start {datetime.now(timezone.utc).isoformat()} port={args.port}\n")
        out.flush()
        while time.monotonic() < deadline:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                continue
            ts = datetime.now(timezone.utc).isoformat()
            text = data.decode("utf-8", errors="replace").strip()
            line = f"{ts}\t{addr[0]}:{addr[1]}\t{text}\n"
            out.write(line)
            out.flush()
            if not args.no_echo:
                sys.stdout.write(line)
                sys.stdout.flush()
            count += 1

        out.write(f"# end packets={count}\n")

    sock.close()
    print(f"wrote {count} packet(s) to {args.output}", file=sys.stderr)


if __name__ == "__main__":
    main()
