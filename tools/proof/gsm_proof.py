#!/usr/bin/env python3
"""Run and log the T-Call A7670 GSM webpage proof over the WiFi console."""

from __future__ import annotations

import argparse
import datetime as dt
import pathlib
import socket
import sys
import time


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

PASS_MARKER = "GSM PROOF PASS"
FAIL_MARKER = "GSM PROOF FAIL"


def make_log_path(log_dir: pathlib.Path, host: str) -> pathlib.Path:
    safe_host = "".join(ch if ch.isalnum() or ch in ".-" else "_" for ch in host)
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    return log_dir / f"gsm_proof_{safe_host}_{stamp}.log"


def main() -> int:
    parser = argparse.ArgumentParser(description="Run GSM webpage proof on the T-Call A7670 WiFi console.")
    parser.add_argument("host", nargs="?", default="tcall-a7670-v10.local",
                        help="Board hostname or IP address")
    parser.add_argument("-p", "--port", type=int, default=23, help="TCP console port")
    parser.add_argument("--timeout", type=int, default=180, help="EPS registration timeout seconds")
    parser.add_argument("--web-host", default="example.com", help="HTTP host fetched through GSM")
    parser.add_argument("--web-path", default="/", help="HTTP path fetched through GSM")
    parser.add_argument("--connect-timeout", type=float, default=10.0, help="TCP connect timeout seconds")
    parser.add_argument("--log-dir", default="logs", help="Directory for proof logs")
    args = parser.parse_args()

    web_path = args.web_path if args.web_path.startswith("/") else f"/{args.web_path}"
    log_dir = pathlib.Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = make_log_path(log_dir, args.host)

    command = f"gsm prove {args.timeout} {args.web_host} {web_path}\n".encode("utf-8")
    deadline = time.monotonic() + (args.timeout * 3) + 600
    status = 2

    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.connect_timeout)
    except OSError as exc:
        print(f"connect failed: {exc}", file=sys.stderr)
        return 1

    sock.settimeout(1.0)
    with sock, log_path.open("w", encoding="utf-8", newline="") as log:
        header = (
            f"# GSM proof host={args.host} port={args.port} timeout_s={args.timeout} "
            f"web_host={args.web_host} web_path={web_path} started={dt.datetime.now().isoformat()}\n"
        )
        print(header, end="")
        log.write(header)

        time.sleep(0.3)
        sock.sendall(command)

        while time.monotonic() < deadline:
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError as exc:
                line = f"\n# socket error: {exc}\n"
                print(line, end="")
                log.write(line)
                break

            if not data:
                break

            text = data.decode("utf-8", errors="replace")
            print(text, end="", flush=True)
            log.write(text)
            log.flush()

            if PASS_MARKER in text:
                status = 0
                break
            if FAIL_MARKER in text:
                status = 1
                break

        if status == 2:
            line = "\nGSM PROOF FAIL reason=client_timeout_or_disconnect\n"
            print(line, end="")
            log.write(line)
            status = 1

        footer = f"\n# log={log_path.resolve()}\n"
        print(footer, end="")
        log.write(footer)

    return status


if __name__ == "__main__":
    raise SystemExit(main())
