#!/usr/bin/env python3
"""Small dependency-free MQTT subscriber for the T-Call telemetry topics."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass


DEFAULT_TOPIC = "tcall/a7670/v10/#"
DEFAULT_CLIENT_ID = "tcall-mqtt-testclient"


class MqttError(RuntimeError):
    pass


@dataclass
class MqttMessage:
    topic: str
    payload: bytes
    retain: bool

    def text(self) -> str:
        return self.payload.decode("utf-8", errors="replace")


def encode_string(value: str) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) > 65535:
        raise ValueError("MQTT string is too long")
    return struct.pack("!H", len(raw)) + raw


def encode_remaining_length(length: int) -> bytes:
    encoded = bytearray()
    while True:
        digit = length % 128
        length //= 128
        if length > 0:
            digit |= 0x80
        encoded.append(digit)
        if length == 0:
            return bytes(encoded)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise MqttError("connection closed by broker")
        data.extend(chunk)
    return bytes(data)


def recv_packet(sock: socket.socket) -> tuple[int, int, bytes]:
    first = recv_exact(sock, 1)[0]
    multiplier = 1
    remaining = 0
    while True:
        digit = recv_exact(sock, 1)[0]
        remaining += (digit & 127) * multiplier
        if (digit & 128) == 0:
            break
        multiplier *= 128
        if multiplier > 128 * 128 * 128:
            raise MqttError("malformed remaining length")
    return first >> 4, first & 0x0F, recv_exact(sock, remaining)


def send_packet(sock: socket.socket, packet_type: int, flags: int, payload: bytes) -> None:
    sock.sendall(bytes([(packet_type << 4) | flags]) + encode_remaining_length(len(payload)) + payload)


def connect(
    host: str,
    port: int,
    client_id: str,
    username: str | None = None,
    password: str | None = None,
    keepalive: int = 30,
) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(1.0)

    flags = 0x02
    variable = encode_string("MQTT") + bytes([4, flags, keepalive >> 8, keepalive & 0xFF])
    payload = encode_string(client_id)
    if username is not None:
        flags |= 0x80
        payload += encode_string(username)
    if password is not None:
        flags |= 0x40
        payload += encode_string(password)

    variable = encode_string("MQTT") + bytes([4, flags, keepalive >> 8, keepalive & 0xFF])
    send_packet(sock, 1, 0, variable + payload)

    packet_type, _, payload = recv_packet(sock)
    if packet_type != 2 or len(payload) != 2:
        raise MqttError("broker did not return CONNACK")
    if payload[1] != 0:
        raise MqttError(f"broker rejected connection with CONNACK code {payload[1]}")
    return sock


def subscribe(sock: socket.socket, topic: str, packet_id: int = 1) -> list[MqttMessage]:
    payload = struct.pack("!H", packet_id) + encode_string(topic) + b"\x00"
    send_packet(sock, 8, 2, payload)
    pending: list[MqttMessage] = []
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            packet_type, flags, response = recv_packet(sock)
        except socket.timeout:
            continue
        if packet_type == 9 and len(response) >= 3:
            if response[-1] == 0x80:
                raise MqttError(f"broker rejected subscription to {topic}")
            return pending
        if packet_type == 3:
            pending.append(parse_publish(flags, response))
            continue
    raise MqttError("broker did not return SUBACK")


def publish(host: str, port: int, topic: str, payload: str, username: str | None, password: str | None) -> None:
    sock = connect(host, port, f"{DEFAULT_CLIENT_ID}-publisher", username, password)
    try:
        send_packet(sock, 3, 0, encode_string(topic) + payload.encode("utf-8"))
        send_packet(sock, 14, 0, b"")
    finally:
        sock.close()


def parse_publish(flags: int, payload: bytes) -> MqttMessage:
    if len(payload) < 2:
        raise MqttError("malformed PUBLISH packet")
    topic_len = struct.unpack("!H", payload[:2])[0]
    topic_end = 2 + topic_len
    if len(payload) < topic_end:
        raise MqttError("malformed PUBLISH topic")
    topic = payload[2:topic_end].decode("utf-8", errors="replace")
    return MqttMessage(topic=topic, payload=payload[topic_end:], retain=bool(flags & 0x01))


def format_message(message: MqttMessage) -> str:
    timestamp = dt.datetime.now().isoformat(timespec="seconds")
    text = message.text()
    try:
        parsed = json.loads(text)
        text = json.dumps(parsed, ensure_ascii=False, separators=(",", ":"))
    except json.JSONDecodeError:
        pass
    retain = " retained" if message.retain else ""
    return f"{timestamp} {message.topic}{retain} {text}"


def subscribe_loop(args: argparse.Namespace) -> int:
    os.makedirs(os.path.dirname(args.log) or ".", exist_ok=True) if args.log else None
    deadline = time.monotonic() + args.timeout if args.timeout > 0 else None
    last_ping = time.monotonic()
    seen = 0

    sock = connect(args.host, args.port, args.client_id, args.username, args.password)
    try:
        pending_messages = subscribe(sock, args.topic)
        print(f"Subscribed to mqtt://{args.host}:{args.port}/{args.topic}")

        if args.self_test:
            publish(args.host, args.port, args.self_test_topic, args.self_test_payload, args.username, args.password)

        log_file = open(args.log, "a", encoding="utf-8") if args.log else None
        try:
            for message in pending_messages:
                line = format_message(message)
                print(line, flush=True)
                if log_file:
                    log_file.write(line + "\n")
                    log_file.flush()
                seen += 1
                if seen >= args.count:
                    return 0

            while True:
                if deadline is not None and time.monotonic() >= deadline:
                    return 0 if seen >= args.count else 2

                if time.monotonic() - last_ping >= 15:
                    send_packet(sock, 12, 0, b"")
                    last_ping = time.monotonic()

                try:
                    packet_type, flags, payload = recv_packet(sock)
                except socket.timeout:
                    continue

                if packet_type == 3:
                    line = format_message(parse_publish(flags, payload))
                    print(line, flush=True)
                    if log_file:
                        log_file.write(line + "\n")
                        log_file.flush()
                    seen += 1
                    if seen >= args.count:
                        return 0
                elif packet_type == 13:
                    continue
        finally:
            if log_file:
                log_file.close()
    finally:
        try:
            send_packet(sock, 14, 0, b"")
        except OSError:
            pass
        sock.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Receive MQTT telemetry from the T-Call A7670 firmware.")
    parser.add_argument("host", nargs="?", default="127.0.0.1", help="MQTT broker host or IP")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port")
    parser.add_argument("--topic", default=DEFAULT_TOPIC, help="Topic filter to subscribe to")
    parser.add_argument("--client-id", default=DEFAULT_CLIENT_ID, help="MQTT client id")
    parser.add_argument("--username", default=None, help="MQTT username")
    parser.add_argument("--password", default=None, help="MQTT password")
    parser.add_argument("--timeout", type=int, default=60, help="Seconds to wait; 0 waits forever")
    parser.add_argument("--count", type=int, default=1, help="Exit after this many messages")
    parser.add_argument("--log", default="", help="Optional log file path")
    parser.add_argument("--self-test", action="store_true", help="Publish one local test message after subscribing")
    parser.add_argument("--self-test-topic", default="tcall/a7670/v10/selftest", help="Topic used by --self-test")
    parser.add_argument("--self-test-payload", default='{"source":"mqtt_subscribe.py","ok":true}', help="Payload used by --self-test")
    return parser.parse_args()


def main() -> int:
    try:
        return subscribe_loop(parse_args())
    except (OSError, MqttError, ValueError) as exc:
        print(f"MQTT testclient failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
