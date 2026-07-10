#!/usr/bin/env python3
"""Interactive TCP console client for the T-Call A7670 firmware."""

from __future__ import annotations

import argparse
import socket
import sys
import threading
import time


def receive_loop(sock: socket.socket, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            data = sock.recv(4096)
        except OSError:
            break
        if not data:
            break
        text = data.decode("utf-8", errors="replace")
        print(text, end="", flush=True)
    stop.set()


def main() -> int:
    parser = argparse.ArgumentParser(description="Connect to the T-Call A7670 WiFi console.")
    parser.add_argument("host", help="Board IP address or hostname, for example tcall-a7670-v10.local")
    parser.add_argument("-p", "--port", type=int, default=23, help="TCP console port (default: 23)")
    parser.add_argument("-t", "--timeout", type=float, default=10.0, help="Connect timeout seconds")
    parser.add_argument("-c", "--command", action="append", help="Command to send, repeatable")
    parser.add_argument("--read-window", type=float, default=2.0, help="Seconds to wait after scripted commands")
    args = parser.parse_args()

    stop = threading.Event()
    try:
        sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    except OSError as exc:
        print(f"connect failed: {exc}", file=sys.stderr)
        return 1

    try:
        sock.settimeout(None)
        receiver = threading.Thread(target=receive_loop, args=(sock, stop), daemon=True)
        receiver.start()

        if args.command:
            time.sleep(0.3)
            for command in args.command:
                sock.sendall(command.rstrip("\r\n").encode("utf-8") + b"\n")
                time.sleep(args.read_window)
        else:
            while not stop.is_set():
                line = sys.stdin.readline()
                if line == "":
                    break
                if line.strip().lower() in {"quit", "exit"}:
                    break
                sock.sendall(line.rstrip("\r\n").encode("utf-8") + b"\n")
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
