#!/usr/bin/env python3
"""RemoteDeviceManager WebUI, HTTP file host, and MQTT controller."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import select
import socket
import struct
import threading
import time
import urllib.parse
import urllib.request
import uuid
import webbrowser
import zlib
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class DualStackThreadingHTTPServer(ThreadingHTTPServer):
    address_family = socket.AF_INET6

    def server_bind(self):
        if hasattr(socket, "IPV6_V6ONLY"):
            try:
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            except OSError:
                pass
        super().server_bind()


class MuxingThreadingHTTPServer(ThreadingHTTPServer):
    def finish_request(self, request, client_address):
        if is_mqtt_connect(request):
            proxy_mqtt_socket(request, self.mqtt_upstream_host, self.mqtt_upstream_port)
            return
        self.RequestHandlerClass(request, client_address, self)


class DualStackMuxingThreadingHTTPServer(MuxingThreadingHTTPServer):
    address_family = socket.AF_INET6

    def server_bind(self):
        if hasattr(socket, "IPV6_V6ONLY"):
            try:
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            except OSError:
                pass
        super().server_bind()


def is_mqtt_connect(sock: socket.socket) -> bool:
    try:
        sock.settimeout(1.0)
        first = sock.recv(1, socket.MSG_PEEK)
        return first == b"\x10"
    except OSError:
        return False


def proxy_mqtt_socket(client: socket.socket, upstream_host: str, upstream_port: int) -> None:
    upstream = None
    try:
        upstream = socket.create_connection((upstream_host, upstream_port), timeout=3.0)
        client.settimeout(None)
        upstream.settimeout(None)
        sockets = [client, upstream]
        while True:
            readable, _, _ = select.select(sockets, [], [], 60)
            if not readable:
                return
            for source in readable:
                data = source.recv(4096)
                if not data:
                    return
                target = upstream if source is client else client
                target.sendall(data)
    except OSError:
        return
    finally:
        if upstream is not None:
            try:
                upstream.close()
            except OSError:
                pass


def read_duckdns_domain() -> str:
    env_path = Path(__file__).resolve().parents[1] / "dyndns" / "duckdns.env"
    if not env_path.is_file():
        return ""
    for line in env_path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if line.startswith("DUCKDNS_DOMAIN="):
            value = line.split("=", 1)[1].strip()
            if value:
                return value if "." in value else f"{value}.duckdns.org"
    return ""


def host_from_url(url: str) -> str:
    parsed = urllib.parse.urlparse(url)
    return parsed.hostname or ""


def encstr(value: str) -> bytes:
    raw = value.encode("utf-8")
    return struct.pack("!H", len(raw)) + raw


def enclen(length: int) -> bytes:
    out = bytearray()
    while True:
        digit = length % 128
        length //= 128
        if length:
            digit |= 0x80
        out.append(digit)
        if not length:
            return bytes(out)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise OSError("MQTT connection closed")
        data.extend(chunk)
    return bytes(data)


def recv_packet(sock: socket.socket) -> tuple[int, int, bytes]:
    first = recv_exact(sock, 1)[0]
    multiplier = 1
    remaining = 0
    while True:
        digit = recv_exact(sock, 1)[0]
        remaining += (digit & 127) * multiplier
        if not digit & 128:
            break
        multiplier *= 128
    return first >> 4, first & 0x0F, recv_exact(sock, remaining)


def parse_publish(flags: int, payload: bytes) -> tuple[str, str, bool]:
    topic_len = struct.unpack("!H", payload[:2])[0]
    topic_end = 2 + topic_len
    topic = payload[2:topic_end].decode("utf-8", errors="replace")
    text = payload[topic_end:].decode("utf-8", errors="replace")
    return topic, text, bool(flags & 1)


class MqttClient:
    def __init__(self, host: str, port: int, client_id: str, username: str | None, password: str | None):
        self.host = host
        self.port = port
        self.client_id = client_id
        self.username = username
        self.password = password
        self.sock: socket.socket | None = None
        self.packet_id = 1
        self.lock = threading.Lock()

    def connect(self) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=10)
        sock.settimeout(1.0)
        flags = 0x02
        payload = encstr(self.client_id)
        if self.username:
            flags |= 0x80
            payload += encstr(self.username)
        if self.password:
            flags |= 0x40
            payload += encstr(self.password)
        variable = encstr("MQTT") + bytes([4, flags, 0, 30])
        sock.sendall(b"\x10" + enclen(len(variable) + len(payload)) + variable + payload)
        packet_type, _, response = recv_packet(sock)
        if packet_type != 2 or response[-1] != 0:
            raise OSError("MQTT CONNACK failed")
        self.sock = sock

    def publish(self, topic: str, payload: str, retain: bool = False) -> None:
        body = encstr(topic) + payload.encode("utf-8")
        with self.lock:
            if self.sock is None:
                raise OSError("MQTT is not connected")
            self.sock.sendall(bytes([0x31 if retain else 0x30]) + enclen(len(body)) + body)

    def subscribe(self, topic: str) -> None:
        packet_id = self.packet_id
        self.packet_id += 1
        body = struct.pack("!H", packet_id) + encstr(topic) + b"\x00"
        with self.lock:
            if self.sock is None:
                raise OSError("MQTT is not connected")
            self.sock.sendall(b"\x82" + enclen(len(body)) + body)

    def read(self) -> tuple[str, str, bool] | None:
        if self.sock is None:
            return None
        try:
            packet_type, flags, payload = recv_packet(self.sock)
        except socket.timeout:
            return None
        if packet_type == 3:
            return parse_publish(flags, payload)
        if packet_type == 13:
            return None
        return None

    def ping(self) -> None:
        with self.lock:
            if self.sock:
                self.sock.sendall(b"\xC0\x00")


@dataclass
class AppState:
    args: argparse.Namespace
    mqtt: MqttClient
    root: Path
    hosted: Path
    uploads: Path
    assets: Path
    messages: list[dict] = field(default_factory=list)
    retained: dict[str, object] = field(default_factory=dict)
    server_checks: dict[str, object] = field(default_factory=dict)
    server_checks_ts: float = 0.0
    transfer_stats: dict[str, object] = field(default_factory=lambda: {
        "wifi": {"mqtt": {"up": 0, "down": 0}, "http": {"up": 0, "down": 0}},
        "lte": {"mqtt": {"up": 0, "down": 0}, "http": {"up": 0, "down": 0}},
    })
    lock: threading.Lock = field(default_factory=threading.Lock)

    def topic(self, suffix: str) -> str:
        return f"{self.args.topic_prefix.rstrip('/')}/{suffix}"

    def publish_job(self, suffix: str, payload: dict) -> dict:
        payload.setdefault("schema", "rdm-1")
        payload.setdefault("device_id", self.args.device)
        payload.setdefault("msg_id", f"msg-{uuid.uuid4().hex[:12]}")
        payload.setdefault("job_id", f"job-{uuid.uuid4().hex[:12]}")
        payload.setdefault("created_at", dt.datetime.now(dt.UTC).isoformat())
        encoded = json.dumps(payload, separators=(",", ":"))
        self.mqtt.publish(self.topic(suffix), encoded)
        self.add_transfer(self.infer_link(), "mqtt", "down", len(encoded.encode("utf-8")))
        return payload

    def publish_control(self, suffix: str, payload: dict) -> dict:
        encoded = json.dumps(payload, separators=(",", ":"))
        self.mqtt.publish(self.topic(suffix), encoded)
        self.add_transfer(self.infer_link(), "mqtt", "down", len(encoded.encode("utf-8")))
        return payload

    def record(self, topic: str, payload: str, retain: bool) -> None:
        try:
            parsed: object = json.loads(payload)
        except json.JSONDecodeError:
            parsed = payload
        topic_prefix = self.args.topic_prefix.rstrip("/") + "/"
        suffix = topic.removeprefix(topic_prefix) if topic.startswith(topic_prefix) else topic
        if suffix == "rdm/alive":
            parsed = self._normalize_alive(parsed)
        with self.lock:
            self.messages.append({"ts": time.time(), "topic": topic, "payload": parsed, "retain": retain})
            self.messages = self.messages[-200:]
            self.retained[topic] = parsed
            if suffix not in {"console/cmd", "fs/jobs", "ota/jobs", "rdm/alive/request", "rdm/transport/set"}:
                link = self._infer_link_unlocked(parsed)
                self._add_transfer_unlocked(link, "mqtt", "up", len(payload.encode("utf-8")))

    def _normalize_alive(self, payload: object) -> object:
        if not isinstance(payload, dict) or "w" not in payload:
            return payload

        def boolish(value: object) -> bool:
            return value is True or value == 1 or value == "1"

        def link(raw: object) -> dict[str, object]:
            src = raw if isinstance(raw, dict) else {}
            return {
                "connected": boolish(src.get("c")),
                "ip": str(src.get("i", "")),
                "rssi": int(src.get("r", 0) or 0),
                "mqtt": boolish(src.get("m")),
                "http": boolish(src.get("h")),
            }

        active = str(payload.get("a", "w"))
        return {
            "schema": "rdm-1",
            "device_id": self.args.device,
            "request_id": str(payload.get("r", "")),
            "uptime_ms": int(payload.get("u", 0) or 0),
            "active_link": "lte" if active == "l" else "wifi",
            "transport": str(payload.get("t", "")),
            "sleep_state": str(payload.get("s", "awake")),
            "wifi": link(payload.get("w")),
            "lte": link(payload.get("l")),
        }

    def _infer_link_unlocked(self, payload: object | None = None) -> str:
        if isinstance(payload, dict):
            active = str(payload.get("active_link", "")).lower()
            if active in ("wifi", "lte"):
                return active
            transport = str(payload.get("transport", "")).lower()
            if "lte" in transport or "cellular" in transport or "simcom" in transport:
                return "lte"
            if "wifi" in transport:
                return "wifi"
        for suffix in ("rdm/alive", "rdm/state", "status"):
            retained = self.retained.get(self.topic(suffix))
            if not isinstance(retained, dict):
                continue
            active = str(retained.get("active_link", "")).lower()
            if active in ("wifi", "lte"):
                return active
            transport = str(retained.get("transport", retained.get("mqtt_transport", ""))).lower()
            if "lte" in transport or "cellular" in transport or "simcom" in transport:
                return "lte"
            if "wifi" in transport:
                return "wifi"
        return "wifi"

    def infer_link(self, payload: object | None = None) -> str:
        with self.lock:
            return self._infer_link_unlocked(payload)

    def _add_transfer_unlocked(self, link: str, protocol: str, direction: str, byte_count: int) -> None:
        if link not in ("wifi", "lte") or protocol not in ("mqtt", "http") or direction not in ("up", "down"):
            return
        bucket = self.transfer_stats.setdefault(link, {}).setdefault(protocol, {})
        bucket[direction] = int(bucket.get(direction, 0)) + max(0, int(byte_count))

    def add_transfer(self, link: str, protocol: str, direction: str, byte_count: int) -> None:
        with self.lock:
            self._add_transfer_unlocked(link, protocol, direction, byte_count)

    def latest_payload(self, suffix: str) -> object | None:
        topic = self.topic(suffix)
        with self.lock:
            for message in reversed(self.messages):
                if message.get("topic") == topic:
                    return message.get("payload")
        return None

    def get_server_checks(self) -> dict[str, object]:
        with self.lock:
            if time.time() - self.server_checks_ts < 5:
                return self.server_checks

        checks = {
            "http_lan": http_status_check(self.args.local_http_url),
            "http_public": http_status_check(self.args.public_http_url),
            "mqtt_lan": mqtt_status_check(self.args.local_mqtt_host, self.args.mqtt_port),
            "mqtt_public": mqtt_status_check(self.args.public_mqtt_host, self.args.public_mqtt_port),
        }
        checked_at = time.time()
        for check in checks.values():
            if isinstance(check, dict):
                check["checked_at"] = checked_at
        with self.lock:
            self.server_checks = checks
            self.server_checks_ts = time.time()
            return checks


def http_status_check(base_url: str) -> dict[str, object]:
    if not base_url:
        return {"ok": False, "detail": "not configured"}
    url = base_url.rstrip("/") + "/api/state?server_checks=0"
    try:
        with urllib.request.urlopen(url, timeout=0.8) as response:
            return {"ok": 200 <= response.status < 300, "detail": str(response.status)}
    except Exception as exc:
        return {"ok": False, "detail": type(exc).__name__}


def mqtt_status_check(host: str, port: int) -> dict[str, object]:
    if not host:
        return {"ok": False, "detail": "not configured"}
    client_id = f"rdm-check-{uuid.uuid4().hex[:6]}"
    variable_header = encstr("MQTT") + bytes([4, 2, 0, 10])
    payload = encstr(client_id)
    remaining = enclen(len(variable_header) + len(payload))
    packet = bytes([0x10]) + remaining + variable_header + payload
    last_error = "unreachable"
    try:
        for family, socktype, proto, _, addr in socket.getaddrinfo(host, port, type=socket.SOCK_STREAM):
            try:
                with socket.socket(family, socktype, proto) as sock:
                    sock.settimeout(0.8)
                    sock.connect(addr)
                    sock.sendall(packet)
                    response = sock.recv(4)
                    ok = response == b"\x20\x02\x00\x00"
                    if ok:
                        sock.sendall(b"\xE0\x00")
                    return {"ok": ok, "detail": "connack" if ok else response.hex()}
            except Exception as exc:
                last_error = type(exc).__name__
        return {"ok": False, "detail": last_error}
    except Exception as exc:
        return {"ok": False, "detail": type(exc).__name__}


HTML = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RemoteDeviceManager</title>
<link rel="icon" href="/assets/rdm-icon.svg" type="image/svg+xml">
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
:root{
  --bg:#edf1f3;--panel:#ffffff;--ink:#121a20;--muted:#697784;--line:#cfd8df;
  --nav:#111820;--nav2:#19242d;--teal:#1fa889;--amber:#d9962b;--red:#c94949;
  --blue:#2b6fbd;--soft:#f6f8f9;--code:#0d141a;--codeText:#d7f7ed;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 "Segoe UI",Arial,sans-serif}
button,input{font:inherit}
.app{display:block;min-height:100vh}
.sidebar{position:fixed;left:0;top:0;bottom:0;z-index:800;width:74px;background:var(--nav);color:white;padding:18px 10px;display:flex;flex-direction:column;gap:16px;overflow:hidden;box-shadow:10px 0 28px rgba(17,24,32,.12);transition:width .18s ease,padding .18s ease}
.sidebar:hover,.sidebar:focus-within{width:360px;padding:18px 14px;overflow-y:auto}
.brand{display:flex;align-items:center;gap:12px;padding:2px 4px 12px;border-bottom:1px solid #2b3842;min-width:332px}
.brand img{width:42px;height:42px;flex:0 0 auto}.brand b{display:block;font-size:16px}.brand span{color:#9fb0bd;font-size:12px}
.brand>div,.nav-label,.link-map{opacity:0;visibility:hidden;transition:opacity .12s ease,visibility .12s ease}
.sidebar:hover .brand>div,.sidebar:focus-within .brand>div,.sidebar:hover .nav-label,.sidebar:focus-within .nav-label,.sidebar:hover .link-map,.sidebar:focus-within .link-map{opacity:1;visibility:visible}
.nav{display:grid;gap:6px}.nav button{height:44px;display:grid;grid-template-columns:42px 1fr;align-items:center;gap:8px;text-align:left;border:0;border-radius:8px;background:transparent;color:#cbd6de;padding:0 8px 0 6px;cursor:pointer;min-width:52px}
.nav button.active,.nav button:hover,.nav button:focus-visible{background:var(--nav2);color:white;outline:0}
.nav-icon{width:22px;height:22px;justify-self:center;stroke:currentColor;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}.nav-label{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.link-map{display:grid;gap:12px;min-width:332px}
.node-card{background:#18232c;border:1px solid #2b3842;border-radius:10px;padding:14px;display:grid;gap:12px}
.node-head{display:flex;align-items:center;justify-content:space-between;gap:10px}
.node-kicker{color:#8fa1ae;font-size:11px;text-transform:uppercase;letter-spacing:.06em}
.node-name{font-weight:700;font-size:16px}
.node-grid{display:grid;gap:8px}.node-row{display:grid;grid-template-columns:minmax(82px,.8fr) minmax(0,1.4fr);align-items:center;gap:10px;color:#cbd6de;font-size:13px}.node-value{color:#eef4f8;font-family:Consolas,"Cascadia Mono",monospace;font-size:12px;text-align:right;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.node-card:last-child .node-row{grid-template-columns:minmax(78px,.7fr) minmax(0,1.6fr)}.node-card:last-child .node-value{white-space:normal;word-break:break-word;line-height:1.25;overflow:visible;text-overflow:clip}
.endpoint-value{display:flex;align-items:center;justify-content:flex-end;gap:7px}.fresh-pulse{--pulse:#526575;width:13px;height:13px;border-radius:50%;border:1px solid var(--pulse);position:relative;display:inline-block;flex:0 0 auto}.fresh-pulse:after{content:"";position:absolute;inset:-4px;border-radius:50%;border:1px solid var(--pulse);opacity:.75;animation:pulseCheck 1.6s ease-out infinite}.fresh-pulse.ok{--pulse:var(--teal)}.fresh-pulse.err{--pulse:var(--red);animation:none}.fresh-pulse.err:after{animation:none;opacity:.25}.fresh-pulse.warn{--pulse:var(--amber)}@keyframes pulseCheck{0%{transform:scale(.55);opacity:.9}100%{transform:scale(1.5);opacity:0}}
.transport-choice{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:3px;background:#101922;border:1px solid #263642;border-radius:999px}.transport-choice button{height:28px;min-width:70px;border:0;border-radius:999px;background:transparent;color:#9fb0bd;cursor:pointer}.transport-choice button.active{background:var(--teal);color:white;font-weight:650}.transport-choice button.pending{background:var(--amber);color:#15110a;font-weight:650}.transport-choice button.blocked{color:#647987;cursor:not-allowed}
.transport-list{display:grid;gap:8px}.transport-card{border:1px solid #2b3b47;background:#121c24;border-radius:9px;padding:10px;display:grid;gap:8px}.transport-top{display:flex;align-items:center;justify-content:space-between;gap:8px}.transport-name{display:flex;align-items:center;gap:7px;color:#f4f8fb;font-weight:650}.transport-ip{font-family:Consolas,"Cascadia Mono",monospace;font-size:12px;color:#dfe8ee;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.check-pair{display:grid;grid-template-columns:1fr 1fr;gap:7px}.check{display:flex;align-items:center;justify-content:space-between;gap:6px;color:#9fb0bd;font-size:11px;border:1px solid #2b3b47;border-radius:7px;padding:5px 7px;background:#0f171f}
.alive-row{display:flex;align-items:center;justify-content:space-between;gap:10px}.alive-ring{--p:0;width:24px;height:24px;border-radius:50%;background:conic-gradient(var(--teal) calc(var(--p)*1%),#31404b 0);display:grid;place-items:center;flex:0 0 auto}.alive-ring:after{content:"";width:15px;height:15px;border-radius:50%;background:#18232c}.alive-text{color:#9fb0bd;font-size:12px}
.link-lines{display:grid;grid-template-columns:1fr 1fr;gap:14px;padding:0 20px;min-height:154px}
.protocol-link{position:relative;display:grid;place-items:center;min-height:154px;color:#9fb0bd}
.protocol-link svg{position:absolute;inset:0;width:100%;height:100%;overflow:visible}
.lane-path{fill:none;stroke-width:2;stroke-linecap:round;stroke-dasharray:4 5}
.lane-path.up{stroke:#82a0b3}.lane-path.down{stroke:var(--teal)}
.protocol-label{position:relative;z-index:1;min-width:86px;text-align:center;border:1px solid #40515d;border-radius:999px;padding:5px 11px;background:#121c24;color:#f4f8fb;font-size:12px;font-weight:650;letter-spacing:.04em;box-shadow:0 0 0 5px #111820}
.lane-stats{position:absolute;z-index:1;left:50%;bottom:8px;transform:translateX(-50%);display:grid;grid-template-columns:1fr 1fr;gap:6px;width:min(148px,100%);font-size:10px;color:#9fb0bd}
.lane-stats span{border:1px solid #2f4351;background:#101922;border-radius:999px;padding:3px 6px;text-align:center;white-space:nowrap}
.lane-stats b{color:#eef4f8;font-family:Consolas,"Cascadia Mono",monospace;font-weight:650}
.dot{width:9px;height:9px;border-radius:50%;background:#526575;display:inline-block}.dot.ok{background:var(--teal)}.dot.warn{background:var(--amber)}.dot.err{background:var(--red)}
.main{min-width:0;padding:24px 28px 32px 102px}
.topbar{display:flex;align-items:flex-start;justify-content:space-between;gap:18px;margin-bottom:16px}
h1{font-size:22px;margin:0 0 4px}.subtitle{color:var(--muted)}
.actions{display:flex;gap:8px;flex-wrap:wrap}.btn{border:1px solid var(--line);background:var(--panel);color:var(--ink);border-radius:8px;height:36px;padding:0 12px;cursor:pointer}
.btn.primary{background:var(--teal);border-color:var(--teal);color:white}.btn.danger{border-color:#e0b8b8;color:#9e2929}.btn:hover{filter:brightness(.97)}
.grid{display:grid;gap:12px}.metrics{grid-template-columns:repeat(6,minmax(130px,1fr));margin-bottom:14px}
.metric,.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px}
.metric{padding:13px 14px;min-height:82px}.label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.04em}.value{font-size:20px;font-weight:650;margin-top:5px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.panel{padding:14px}.panel-head{display:flex;align-items:center;justify-content:flex-end;gap:10px;margin-bottom:12px}.panel-head:empty{display:none}.panel-title{font-weight:650}.panel-note{color:var(--muted);font-size:12px}
.view{display:none}.view.active{display:block}.split{display:grid;grid-template-columns:minmax(0,1.35fr) minmax(320px,.65fr);gap:12px}.device-grid{display:grid;grid-template-columns:repeat(3,minmax(240px,1fr));gap:12px}
pre,.console{margin:0;background:var(--code);color:var(--codeText);border-radius:8px;padding:12px;overflow:auto;white-space:pre-wrap}
.console{height:470px;font:13px/1.45 Consolas,"Cascadia Mono",monospace}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.row.tight{margin-top:10px}
input[type=text],input:not([type]){height:36px;border:1px solid var(--line);border-radius:8px;padding:0 10px;background:white;min-width:0}
.cmd{flex:1}.table-wrap{overflow:auto;border:1px solid var(--line);border-radius:8px;background:white}
.console-input-wrap{position:relative;flex:1;min-width:240px}.console-input-wrap .cmd{width:100%;box-sizing:border-box}.command-suggestions{position:absolute;left:0;right:0;bottom:42px;z-index:700;background:white;border:1px solid var(--line);border-radius:8px;box-shadow:0 14px 34px rgba(25,39,52,.14);max-height:260px;overflow:auto;padding:6px;display:none}.command-suggestions.show{display:block}.command-suggestion{display:grid;grid-template-columns:minmax(120px,.45fr) minmax(0,1fr);gap:10px;width:100%;border:0;background:transparent;border-radius:6px;padding:8px 9px;text-align:left;cursor:pointer;color:var(--ink)}.command-suggestion:hover,.command-suggestion.active{background:#eef5f7}.command-suggestion code{font-family:Consolas,"Cascadia Mono",monospace;font-weight:700;color:#163040}.command-suggestion span{color:var(--muted);font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
table{width:100%;border-collapse:collapse}th,td{padding:10px 12px;border-bottom:1px solid #e4eaee;text-align:left;vertical-align:middle}th{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.04em;background:#f7f9fa}tr:last-child td{border-bottom:0}.path{font-family:Consolas,"Cascadia Mono",monospace}
.drop{border:1px dashed #8ea1ad;background:#f9fbfb;border-radius:8px;padding:18px;text-align:center;color:var(--muted);margin:10px 0;cursor:pointer}.drop.drag{border-color:var(--teal);background:#edf9f6;color:#146c5a}
.file-sync{display:inline-flex;align-items:center;gap:8px;color:var(--muted);font-size:12px}.sync-ring{width:18px;height:18px;border-radius:50%;border:2px solid #d4dde3;border-top-color:var(--teal);animation:spin .9s linear infinite}.sync-ring.idle{animation:none;border-top-color:#d4dde3}@keyframes spin{to{transform:rotate(360deg)}}
.file-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(152px,1fr));gap:14px;margin-top:14px}.file-card{position:relative;min-height:154px;border:1px solid var(--line);border-radius:8px;background:white;padding:13px;display:grid;grid-template-rows:auto minmax(38px,1fr) auto;gap:9px;cursor:default;user-select:none;overflow:hidden}.file-card:hover{border-color:#9fb0bd;box-shadow:0 6px 18px rgba(25,39,52,.08)}.file-card.pending{opacity:.45}.file-icon{width:50px;height:60px;border:1px solid #bed0da;border-radius:7px 7px 5px 5px;background:#f7fafb;display:grid;place-items:end center;padding-bottom:8px;color:#355062;font-weight:750;font-size:12px;letter-spacing:.04em;position:relative}.file-icon:before{content:"";position:absolute;right:-1px;top:-1px;border-left:14px solid transparent;border-bottom:14px solid #dce7ed}.file-icon.bin{background:#eefaf6;color:#14765f}.file-icon.json,.file-icon.cfg{background:#eef5ff;color:#2b6fbd}.file-icon.txt,.file-icon.log{background:#fff8e8;color:#805400}.file-icon.img{background:#fff1f1;color:#982626}.file-name{font-weight:650;line-height:1.22;overflow:hidden;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;word-break:break-word}.file-meta{color:var(--muted);font-size:12px}.file-progress{position:absolute;inset:0;display:grid;place-items:center;background:rgba(255,255,255,.62);border-radius:8px}.progress-ring{--p:0;width:46px;height:46px;border-radius:50%;background:conic-gradient(var(--teal) calc(var(--p)*1%),#d4dde3 0);display:grid;place-items:center;color:#102018;font-size:11px;font-weight:700}.progress-ring:after{content:"";position:absolute;width:31px;height:31px;border-radius:50%;background:white}.progress-ring span{position:relative;z-index:1}.empty-files{grid-column:1/-1;border:1px dashed #c7d4dc;border-radius:8px;background:#fbfcfd;padding:28px;text-align:center;color:var(--muted);margin-top:14px}.context-menu{position:fixed;z-index:900;background:#111820;color:white;border:1px solid #2b3842;border-radius:8px;box-shadow:0 16px 40px rgba(0,0,0,.24);padding:6px;min-width:190px;display:none}.context-menu.show{display:block}.context-menu button{display:block;width:100%;height:34px;text-align:left;border:0;background:transparent;color:white;border-radius:6px;padding:0 10px;cursor:pointer}.context-menu button:hover{background:#1d2a35}.context-menu button.danger{color:#ffb6b6}
.status-list{display:grid;gap:8px}.status-item{display:flex;align-items:center;justify-content:space-between;gap:12px;border-bottom:1px solid #e4eaee;padding:8px 0}.status-item:last-child{border-bottom:0}
.badge{display:inline-flex;align-items:center;gap:6px;border:1px solid var(--line);border-radius:999px;padding:4px 8px;background:#fff;font-size:12px}.badge.ok{border-color:#a8d8ca;color:#14765f;background:#eefaf6}.badge.warn{border-color:#e8ce97;color:#805400;background:#fff8e8}.badge.err{border-color:#e2b0b0;color:#982626;background:#fff1f1}
.messages{height:430px}
.map-shell{position:relative;min-height:620px;border:1px solid var(--line);border-radius:8px;overflow:hidden;background:#dfe7e9}.vehicle-map{height:620px;width:100%}.map-overlay{position:absolute;right:14px;top:14px;z-index:500;display:grid;gap:8px;width:min(320px,calc(100% - 92px));max-width:320px}.map-card{background:rgba(17,24,32,.92);color:#f7fbfd;border:1px solid #314552;border-radius:8px;padding:11px 12px;box-shadow:0 8px 24px rgba(0,0,0,.18)}.speed-value{font-size:28px;font-weight:750;line-height:1}.speed-unit{color:#9fb0bd;font-size:12px;margin-left:4px}.map-meta{display:grid;gap:4px;color:#c6d4dd;font-size:12px}.vehicle-marker{width:28px;height:28px;border-radius:50%;background:#1fa889;border:3px solid #fff;box-shadow:0 3px 12px rgba(0,0,0,.35);position:relative}.vehicle-marker:after{content:"";position:absolute;left:50%;top:-8px;transform:translateX(-50%);border-left:5px solid transparent;border-right:5px solid transparent;border-bottom:9px solid #fff}
.upload-line{display:grid;grid-template-columns:1fr auto;gap:8px}.file-input{border:1px solid var(--line);border-radius:8px;padding:7px;background:white}.file-input.hidden{display:none}
.toast{position:fixed;right:18px;bottom:18px;background:#111820;color:white;border-radius:8px;padding:10px 12px;opacity:0;transform:translateY(10px);transition:.18s}.toast.show{opacity:1;transform:translateY(0)}
@media(max-width:980px){.main{padding:18px 16px 88px}.sidebar{left:0;right:0;top:auto;bottom:0;width:auto;height:64px;padding:7px 8px;display:block;overflow:visible;box-shadow:0 -8px 24px rgba(17,24,32,.16)}.sidebar:hover,.sidebar:focus-within{width:auto;height:64px;padding:7px 8px;overflow:visible}.brand,.link-map{display:none}.nav{height:50px;display:grid;grid-template-columns:repeat(6,1fr);gap:4px}.nav button{height:50px;min-width:0;display:grid;grid-template-columns:1fr;place-items:center;padding:0;border-radius:9px}.nav-icon{width:23px;height:23px}.nav-label{display:none}.metrics{grid-template-columns:repeat(2,minmax(130px,1fr))}.split,.device-grid{grid-template-columns:1fr}.topbar{display:grid}}
</style>
</head>
<body>
<svg aria-hidden="true" style="position:absolute;width:0;height:0;overflow:hidden">
  <symbol id="icon-device" viewBox="0 0 24 24"><rect x="6" y="3" width="12" height="18" rx="2"/><path d="M10 7h4M10 17h4"/></symbol>
  <symbol id="icon-map" viewBox="0 0 24 24"><path d="M9 18l-6 3V6l6-3 6 3 6-3v15l-6 3-6-3z"/><path d="M9 3v15M15 6v15"/></symbol>
  <symbol id="icon-console" viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 9l3 3-3 3M12 15h5"/></symbol>
  <symbol id="icon-files" viewBox="0 0 24 24"><path d="M4 20h16V8h-8l-2-3H4z"/><path d="M4 8h16"/></symbol>
  <symbol id="icon-ota" viewBox="0 0 24 24"><path d="M12 3v12"/><path d="M7 8l5-5 5 5"/><path d="M5 19h14"/></symbol>
  <symbol id="icon-trace" viewBox="0 0 24 24"><path d="M4 7h4l2 10 4-14 2 8h4"/><path d="M4 19h16"/></symbol>
</svg>
<div class="app">
  <aside class="sidebar">
    <div class="brand"><img src="/assets/rdm-icon.svg" alt=""><div><b>RemoteDeviceManager</b><span id="brandDevice">eboxster</span></div></div>
    <nav class="nav" aria-label="Main">
      <button class="active" data-view="device" title="Device"><svg class="nav-icon"><use href="#icon-device"></use></svg><span class="nav-label">Device</span></button>
      <button data-view="map" title="Map"><svg class="nav-icon"><use href="#icon-map"></use></svg><span class="nav-label">Map</span></button>
      <button data-view="console" title="Console"><svg class="nav-icon"><use href="#icon-console"></use></svg><span class="nav-label">Console</span></button>
      <button data-view="files" title="Files"><svg class="nav-icon"><use href="#icon-files"></use></svg><span class="nav-label">Files</span></button>
      <button data-view="ota" title="OTA"><svg class="nav-icon"><use href="#icon-ota"></use></svg><span class="nav-label">OTA</span></button>
      <button data-view="trace" title="MQTT Trace"><svg class="nav-icon"><use href="#icon-trace"></use></svg><span class="nav-label">MQTT Trace</span></button>
    </nav>
    <section class="link-map" aria-label="Connection overview">
      <div class="node-card">
        <div class="node-head"><div><div class="node-kicker">Device</div><div class="node-name" id="deviceNodeName">eboxster</div></div></div>
        <div class="transport-choice" aria-label="Active device transport">
          <button id="wifiModeBtn" onclick="setDeviceTransport('wifi')">WiFi</button>
          <button id="lteModeBtn" onclick="setDeviceTransport('lte')">LTE</button>
        </div>
        <div class="transport-list">
          <div class="transport-card">
            <div class="transport-top"><span class="transport-name"><i id="wifiDot" class="dot"></i> WiFi</span><span class="transport-ip" id="wifiIp">-</span></div>
            <div class="check-pair"><span class="check">MQTT <i id="wifiMqttDot" class="dot"></i></span><span class="check">HTTP <i id="wifiHttpDot" class="dot"></i></span></div>
          </div>
          <div class="transport-card">
            <div class="transport-top"><span class="transport-name"><i id="lteDot" class="dot"></i> LTE</span><span class="transport-ip" id="lteIp">-</span></div>
            <div class="check-pair"><span class="check">MQTT <i id="lteMqttDot" class="dot"></i></span><span class="check">HTTP <i id="lteHttpDot" class="dot"></i></span></div>
          </div>
        </div>
        <div class="alive-row"><span class="alive-text" id="aliveText">alive pending</span><span class="alive-ring" id="aliveRing"></span></div>
      </div>
      <div class="link-lines">
        <div class="protocol-link" id="mqttLane">
          <svg viewBox="0 0 120 154" aria-hidden="true">
            <defs>
              <marker id="mqttUpArrow" markerWidth="7" markerHeight="7" refX="4.5" refY="3.5" orient="auto" markerUnits="strokeWidth"><path d="M0 0 L7 3.5 L0 7 Z" fill="#82a0b3"/></marker>
              <marker id="mqttDownArrow" markerWidth="7" markerHeight="7" refX="4.5" refY="3.5" orient="auto" markerUnits="strokeWidth"><path d="M0 0 L7 3.5 L0 7 Z" fill="#1fa889"/></marker>
            </defs>
            <path class="lane-path up" d="M46 135 C26 108 26 46 46 18" marker-end="url(#mqttUpArrow)"/>
            <path class="lane-path down" d="M74 18 C94 46 94 108 74 135" marker-end="url(#mqttDownArrow)"/>
          </svg>
          <span class="protocol-label">MQTT</span>
          <span class="lane-stats"><span>up <b id="mqttUpBytes">0 B</b></span><span>down <b id="mqttDownBytes">0 B</b></span></span>
        </div>
        <div class="protocol-link" id="httpLane">
          <svg viewBox="0 0 120 154" aria-hidden="true">
            <defs>
              <marker id="httpUpArrow" markerWidth="7" markerHeight="7" refX="4.5" refY="3.5" orient="auto" markerUnits="strokeWidth"><path d="M0 0 L7 3.5 L0 7 Z" fill="#82a0b3"/></marker>
              <marker id="httpDownArrow" markerWidth="7" markerHeight="7" refX="4.5" refY="3.5" orient="auto" markerUnits="strokeWidth"><path d="M0 0 L7 3.5 L0 7 Z" fill="#1fa889"/></marker>
            </defs>
            <path class="lane-path up" d="M46 135 C26 108 26 46 46 18" marker-end="url(#httpUpArrow)"/>
            <path class="lane-path down" d="M74 18 C94 46 94 108 74 135" marker-end="url(#httpDownArrow)"/>
          </svg>
          <span class="protocol-label">HTTP</span>
          <span class="lane-stats"><span>up <b id="httpUpBytes">0 B</b></span><span>down <b id="httpDownBytes">0 B</b></span></span>
        </div>
      </div>
      <div class="node-card">
        <div class="node-head"><div><div class="node-name">Backend</div></div></div>
        <div class="node-grid">
          <div class="node-row"><span><i id="serverHttpLocalDot" class="dot"></i> HTTP LAN</span><span class="node-value endpoint-value"><span id="serverHttpLocal">-</span><span id="serverHttpLocalPulse" class="fresh-pulse"></span></span></div>
          <div class="node-row"><span><i id="serverHttpPublicDot" class="dot"></i> HTTP Public</span><span class="node-value endpoint-value"><span id="serverHttpPublic">-</span><span id="serverHttpPublicPulse" class="fresh-pulse"></span></span></div>
          <div class="node-row"><span><i id="serverMqttLocalDot" class="dot"></i> MQTT LAN</span><span class="node-value endpoint-value"><span id="serverMqttLocal">-</span><span id="serverMqttLocalPulse" class="fresh-pulse"></span></span></div>
          <div class="node-row"><span><i id="serverMqttPublicDot" class="dot"></i> MQTT Public</span><span class="node-value endpoint-value"><span id="serverMqttPublic">-</span><span id="serverMqttPublicPulse" class="fresh-pulse"></span></span></div>
        </div>
      </div>
    </section>
  </aside>
  <main class="main">
    <div class="topbar">
      <div><h1 id="pageTitle">Device</h1><div class="subtitle" id="pageSubtitle">Live remote state over MQTT control plane.</div></div>
    </div>

    <section id="device" class="view active">
      <div class="device-grid">
        <div class="panel"><div class="status-list" id="deviceConnectivity"></div></div>
        <div class="panel"><div class="status-list" id="deviceSystem"></div></div>
        <div class="panel"><div class="status-list" id="deviceGpsSummary"></div></div>
      </div>
    </section>

    <section id="map" class="view">
      <div class="panel">
        <div class="map-shell">
          <div id="vehicleMap" class="vehicle-map"></div>
          <div class="map-overlay">
            <div class="map-card">
              <div><span class="speed-value" id="mapSpeed">0</span><span class="speed-unit">km/h</span></div>
              <div class="map-meta">
                <span id="mapStatus">No GPS fix</span>
                <span id="mapCoords">-</span>
                <span id="mapSatellites">Satellites -</span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <section id="console" class="view">
      <div class="panel">
        <div class="panel-head"><span class="badge" id="consoleBadge">idle</span></div>
        <div class="console" id="consoleOut"></div>
        <div class="row tight">
          <div class="console-input-wrap">
            <div class="command-suggestions" id="commandSuggestions"></div>
            <input class="cmd" id="cmd" onfocus="showCommandSuggestions()" oninput="showCommandSuggestions()" onkeydown="handleCommandKey(event)" placeholder="help" autocomplete="off">
          </div>
          <button class="btn primary" onclick="sendCmd()">Send</button><button class="btn" onclick="clearConsole()">Clear</button>
        </div>
      </div>
    </section>

    <section id="files" class="view">
      <div class="panel files-panel">
        <div class="panel-head">
          <span class="file-sync"><span class="sync-ring" id="fileSyncRing"></span><span id="fileSyncText">syncing</span></span>
        </div>
        <div class="drop" id="drop" onclick="$('filePick').click()">Drop files here or click to choose files</div>
        <input class="file-input hidden" type="file" id="filePick" multiple onchange="uploadPickedFiles()">
        <div class="file-grid" id="fileGrid"></div>
      </div>
    </section>

    <section id="ota" class="view">
      <div class="split">
        <div class="panel">
          <div class="status-list" id="otaSummary"></div>
          <div class="row tight"><button class="btn primary" onclick="githubOta()">Start GitHub OTA</button></div>
          <div class="upload-line" style="margin-top:12px"><input class="file-input" type="file" id="bin" accept=".bin,application/octet-stream"><button class="btn" onclick="uploadBin()">Upload .bin and OTA</button></div>
        </div>
        <div class="panel"><pre id="otaState"></pre></div>
      </div>
    </section>

    <section id="trace" class="view">
      <div class="panel"><pre id="messages" class="messages"></pre></div>
    </section>
  </main>
</div>
<div class="toast" id="toast"></div>
<div class="context-menu" id="fileMenu">
  <button onclick="downloadSelectedFile()">Download</button>
  <button onclick="copySelectedFilePath()">Copy path</button>
  <button class="danger" onclick="deleteSelectedFile()">Delete</button>
</div>
<script>
let state={}, activeView='device', localConsole='', pendingTransport=null, pendingTransportUntil=0, vehicleMap=null, vehicleMarker=null, lastAliveFullAt=0;
let uploadItems={}, fileListRequestedAt=0, fileListBusy=false, selectedFilePath='', longPressTimer=null, fileMenuOpenedAt=0;
const titles={device:['Device','Live remote state over MQTT control plane.'],map:['Map','Vehicle position and speed from GPS telemetry.'],console:['Console','Run firmware commands without opening a serial port.'],files:['Files','SPIFFS file transfer over the RemoteDeviceManager data plane.'],ota:['OTA','Firmware update operations and results.'],trace:['MQTT Trace','Raw controller message buffer.']};
const consoleCommands=[
  ['help','Show all firmware console commands'],
  ['diag','Print modem and board diagnostics'],
  ['at ','Send raw AT command to modem'],
  ['sim status','Show SIM status'],
  ['sim pin-off','Disable SIM PIN'],
  ['sms list all','List all SMS'],
  ['sms list unread','List unread SMS'],
  ['sms list read','List read SMS'],
  ['sms send ','Send SMS: sms send <number> <message>'],
  ['reg','Print and wait for network registration'],
  ['operator status','Show operator registration status'],
  ['operator auto','Select operator automatically'],
  ['operator telekom','Select Telekom operator'],
  ['rat auto','Use automatic RAT selection'],
  ['rat lte','Force LTE RAT'],
  ['data status','Show cellular data status'],
  ['data up','Bring cellular data up'],
  ['data down','Bring cellular data down'],
  ['mqtt status','Show RemoteDeviceManager MQTT status'],
  ['mqtt publish','Publish telemetry test message'],
  ['ota config','Show GitHub OTA configuration'],
  ['ota latest','Check latest GitHub release'],
  ['ota update','Run latest GitHub OTA update'],
  ['gsm prove ','Run LTE HTTP proof test'],
  ['gsm tcp ','Run LTE TCP probe'],
  ['gsm reset','Reset modem data path'],
  ['http ','HTTP GET: http <host> [path]'],
  ['gps status','Show GNSS status'],
  ['gps on','Enable GNSS'],
  ['gps off','Disable GNSS'],
  ['gps raw','Read raw GNSS data'],
  ['gps fix','Read basic GNSS fix'],
  ['gps ex','Read extended GNSS fix'],
  ['gps cache','Show buffered GNSS data'],
  ['gps prove ','Run GNSS proof until fix'],
  ['gps hot','Start hot GNSS mode'],
  ['gps cold','Start cold GNSS mode'],
  ['wifi status','Show WiFi status']
];
let commandSuggestionIndex=-1;
function topic(s){return (state.topic_prefix||'eboxster')+'/'+s}
function $(id){return document.getElementById(id)}
function showToast(text){let t=$('toast');t.textContent=text;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2600)}
function setView(id){activeView=id;document.querySelectorAll('.view').forEach(v=>v.classList.toggle('active',v.id===id));document.querySelectorAll('.nav button').forEach(b=>b.classList.toggle('active',b.dataset.view===id));$('pageTitle').textContent=titles[id][0];$('pageSubtitle').textContent=titles[id][1];hideFileMenu();if(id==='map')setTimeout(()=>{initVehicleMap();renderVehicleMap(retained('gps'))},80);if(id==='files')listFiles(true)}
document.querySelectorAll('.nav button').forEach(b=>b.onclick=()=>setView(b.dataset.view));
document.addEventListener('click',event=>{if(!event.target.closest('.console-input-wrap'))hideCommandSuggestions()});
async function api(path,body){let r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{})});let data=await r.json();if(!r.ok||data.error)throw new Error(data.error||r.statusText);return data}
async function refresh(){state=await (await fetch('/api/state')).json();render()}
function fmtBytes(n){if(n==null)return '-';if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';return (n/1048576).toFixed(2)+' MB'}
function fmtTransfer(n){n=Number(n||0);if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(n<102400?1:0)+' KB';return (n/1048576).toFixed(2)+' MB'}
function fmtUptime(ms){if(!ms)return '-';let s=Math.floor(ms/1000),h=Math.floor(s/3600),m=Math.floor((s%3600)/60);return h?h+'h '+m+'m':m+'m '+(s%60)+'s'}
function fmtAge(ts){if(!ts)return 'checking';let age=Math.max(0,Date.now()/1000-ts);return 'checked '+(age<10?age.toFixed(1)+'s':Math.round(age)+'s')+' ago'}
function checkAgeText(check){return check?(fmtAge(check.checked_at)+' Â· '+(check.detail||'')):'checking'}
function pulseClass(check){return 'fresh-pulse '+(!check?'':(check.ok?'ok':'err'))}
function setPulse(id,check){let el=$(id);el.className=pulseClass(check);el.title=checkAgeText(check)}
function initVehicleMap(){
  if(vehicleMap||!$('vehicleMap'))return;
  if(!window.L){$('mapStatus').textContent='Map library unavailable';return}
  vehicleMap=L.map('vehicleMap',{zoomControl:true,attributionControl:true}).setView([51.1657,10.4515],6);
  L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OpenStreetMap contributors'}).addTo(vehicleMap);
}
function renderVehicleMap(gps){
  const valid=!!(gps&&gps.valid&&gps.has_fix&&Number.isFinite(Number(gps.lat))&&Number.isFinite(Number(gps.lon))&&(Number(gps.lat)!==0||Number(gps.lon)!==0));
  const speedKmh=Math.max(0,Number(gps.speed_mps||0)*3.6);
  $('mapSpeed').textContent=speedKmh.toFixed(speedKmh>=100?0:1);
  $('mapStatus').textContent=valid?'GPS fix live':'No GPS fix';
  $('mapSatellites').textContent='Satellites '+(gps.sat_total??'-');
  if(!valid){$('mapCoords').textContent='-';return}
  const lat=Number(gps.lat), lon=Number(gps.lon);
  $('mapCoords').textContent=lat.toFixed(6)+', '+lon.toFixed(6);
  initVehicleMap();
  if(!vehicleMap)return;
  const pos=[lat,lon];
  if(!vehicleMarker){
    vehicleMarker=L.marker(pos,{icon:L.divIcon({className:'',html:'<div class="vehicle-marker"></div>',iconSize:[28,28],iconAnchor:[14,14]})}).addTo(vehicleMap);
  }else{
    vehicleMarker.setLatLng(pos);
  }
  vehicleMarker.bindPopup('eboxster<br>'+speedKmh.toFixed(1)+' km/h');
  vehicleMap.setView(pos, Math.max(vehicleMap.getZoom(), 15), {animate:true});
  setTimeout(()=>vehicleMap.invalidateSize(),40);
}
function retained(name){return (state.retained||{})[topic(name)]||{}}
function latestMessage(name){let t=topic(name), msgs=(state.messages||[]).filter(m=>m.topic===t);return msgs.length?msgs[msgs.length-1]:null}
function badge(ok,text){return '<span class="badge '+(ok?'ok':'err')+'"><span class="dot '+(ok?'ok':'')+'"></span>'+text+'</span>'}
function dotClass(ok,known=true){return 'dot '+(!known?'':(ok?'ok':'err'))}
function fileExt(path){let name=(path||'').split('/').pop()||'';let idx=name.lastIndexOf('.');return idx>=0?name.slice(idx+1).toLowerCase():''}
function fileIconClass(path){let ext=fileExt(path);if(['bin','ota'].includes(ext))return 'bin';if(['json'].includes(ext))return 'json';if(['cfg','conf','ini','env'].includes(ext))return 'cfg';if(['txt','md','csv'].includes(ext))return 'txt';if(['log'].includes(ext))return 'log';if(['jpg','jpeg','png','gif','webp','svg'].includes(ext))return 'img';return 'file'}
function fileIconLabel(path){let ext=fileExt(path);if(!ext)return 'FILE';if(['jpeg'].includes(ext))return 'JPG';if(['conf'].includes(ext))return 'CFG';return ext.slice(0,4).toUpperCase()}
function escapeHtml(value){return String(value??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]))}
function fileListPayload(){let list=(state.messages||[]).filter(m=>m.topic.endsWith('/fs/result')&&m.payload.files).slice(-1)[0];return list?list.payload.files:[]}
function uploadProgressHtml(item){let p=Math.max(0,Math.min(100,Math.round(item.progress||0)));return '<div class="file-progress"><div class="progress-ring" style="--p:'+p+'"><span>'+p+'%</span></div></div>'}
function fileCardHtml(file,pending){
  const path=file.path||pending.path, name=(path||'').split('/').pop()||path, cls=fileIconClass(path), label=fileIconLabel(path), size=file.size!=null?fmtBytes(file.size):(pending.size!=null?fmtBytes(pending.size):'uploading');
  const progress=pending?uploadProgressHtml(pending):'';
  return '<div class="file-card '+(pending?'pending':'')+'" data-path="'+escapeHtml(path)+'" oncontextmenu="openFileMenu(event,&quot;'+escapeHtml(path)+'&quot;)" onpointerdown="startFileLongPress(event,&quot;'+escapeHtml(path)+'&quot;)" onpointerup="clearFileLongPress()" onpointerleave="clearFileLongPress()"><div class="file-icon '+cls+'">'+label+'</div><div class="file-name">'+escapeHtml(name)+'</div><div class="file-meta">'+escapeHtml(size)+'</div>'+progress+'</div>'
}
function renderFiles(){
  const lastList=(state.messages||[]).filter(m=>m.topic.endsWith('/fs/result')&&m.payload.files).slice(-1)[0];
  if(lastList&&fileListRequestedAt&&lastList.ts*1000>=fileListRequestedAt-100)fileListBusy=false;
  const files=(lastList?lastList.payload.files:[]).slice().sort((a,b)=>String(a.path||'').localeCompare(String(b.path||'')));
  const pending=Object.values(uploadItems);
  const known=new Set(files.map(f=>f.path));
  const cards=files.map(f=>fileCardHtml(f,uploadItems[f.path])).join('')+pending.filter(p=>!known.has(p.path)).map(p=>fileCardHtml({path:p.path,size:p.size},p)).join('');
  $('fileGrid').innerHTML=cards||'<div class="empty-files">No files reported yet. The list updates automatically.</div>';
  const sourceTs=lastList?lastList.ts*1000:fileListRequestedAt;
  const age=sourceTs?Math.max(0,(Date.now()-sourceTs)/1000):0;
  const busy=fileListBusy||pending.length>0;
  $('fileSyncRing').classList.toggle('idle',!busy);
  $('fileSyncText').textContent=busy?'syncing':'updated '+(age?Math.round(age)+'s ago':'automatically');
}
function render(){
  const st=retained('status'), rdm=retained('rdm/state'), gps=retained('gps'), ota=retained('ota/state'), fs=retained('fs/state');
  const aliveMsg=latestMessage('rdm/alive'), alive=aliveMsg?aliveMsg.payload:{};
  const deviceId=rdm.device_id||st.device_id||alive.device_id||'eboxster', transport=(alive.transport||rdm.transport||st.mqtt_transport||'').toLowerCase(), server=state.server||{}, checks=state.server_checks||{};
  $('brandDevice').textContent='remote access';
  $('deviceNodeName').textContent=deviceId;
  const mqttOk=!!(rdm.mqtt_connected??st.mqtt_connected);
  const actualActiveLink=(alive.active_link||(transport.includes('lte')||transport.includes('cellular')||transport.includes('simcom')?'lte':'wifi')).toLowerCase();
  let activeLink=actualActiveLink;
  if(pendingTransport && actualActiveLink===pendingTransport && aliveMsg && Date.now()/1000-aliveMsg.ts<5) pendingTransport=null;
  if(pendingTransport && Date.now()<pendingTransportUntil && actualActiveLink!==pendingTransport) activeLink=pendingTransport;
  const transfer=(state.transfer_stats||{})[activeLink]||{mqtt:{},http:{}};
  const mqttTransfer=transfer.mqtt||{}, httpTransfer=transfer.http||{};
  $('mqttUpBytes').textContent=fmtTransfer(mqttTransfer.up);
  $('mqttDownBytes').textContent=fmtTransfer(mqttTransfer.down);
  $('httpUpBytes').textContent=fmtTransfer(httpTransfer.up);
  $('httpDownBytes').textContent=fmtTransfer(httpTransfer.down);
  $('mqttLane').title=activeLink.toUpperCase()+' MQTT counters. Up is device to backend, down is backend to device.';
  $('httpLane').title=activeLink.toUpperCase()+' HTTP counters. Up is device upload, down is device download.';
  const wifiKnown=!!alive.wifi, lteKnown=!!alive.lte;
  const wifiMqttOk=!!(alive.wifi&&alive.wifi.mqtt);
  const wifiHttpOk=!!(alive.wifi&&alive.wifi.http);
  const lteMqttOk=!!(alive.lte&&alive.lte.mqtt);
  const lteHttpOk=!!(alive.lte&&alive.lte.http);
  const wifiOk=!!(alive.wifi&&alive.wifi.connected);
  const lteOk=!!(alive.lte&&alive.lte.connected);
  $('wifiDot').className=dotClass(wifiOk,wifiKnown); $('lteDot').className=dotClass(lteOk,lteKnown);
  $('wifiMqttDot').className=dotClass(wifiMqttOk,wifiKnown); $('wifiHttpDot').className=dotClass(wifiHttpOk,wifiKnown);
  $('lteMqttDot').className=dotClass(lteMqttOk,lteKnown); $('lteHttpDot').className=dotClass(lteHttpOk,lteKnown);
  $('wifiIp').textContent=(alive.wifi&&alive.wifi.ip)||st.ip||'-'; $('lteIp').textContent=(alive.lte&&alive.lte.ip)||st.lte_ip||st.cellular_ip||'-';
  $('wifiModeBtn').classList.toggle('active', activeLink!=='lte');
  $('lteModeBtn').classList.toggle('active', activeLink==='lte');
  $('wifiModeBtn').classList.toggle('pending', pendingTransport==='wifi');
  $('lteModeBtn').classList.toggle('pending', pendingTransport==='lte');
  const lteSwitchReady=!!(alive.lte&&alive.lte.mqtt);
  $('lteModeBtn').classList.toggle('blocked', activeLink!=='lte'&&!lteSwitchReady);
  $('lteModeBtn').title=activeLink==='lte'?'LTE active':(lteSwitchReady?'Switch control plane to LTE':'LTE MQTT is not reachable; switch blocked to avoid losing control');
  $('wifiModeBtn').title=activeLink!=='lte'?'WiFi active':'Switch control plane to WiFi';
  const aliveAge=aliveMsg?(Date.now()/1000-aliveMsg.ts):999;
  const aliveFresh=aliveMsg&&aliveAge<30;
  const sleepState=(rdm.sleep_state||alive.sleep_state||st.sleep_state||'awake').toLowerCase();
  const alivePct=Math.max(0,Math.min(100,100-(aliveAge/30)*100));
  $('aliveRing').style.setProperty('--p', alivePct);
  $('aliveText').textContent=sleepState==='sleeping'?'sleeping':(aliveFresh?('alive '+aliveAge.toFixed(1)+'s'):'alive timeout');
  $('serverHttpLocalDot').className=dotClass(!!checks.http_lan?.ok,!!checks.http_lan);
  $('serverHttpPublicDot').className=dotClass(!!checks.http_public?.ok,!!checks.http_public);
  $('serverMqttLocalDot').className=dotClass(!!checks.mqtt_lan?.ok,!!checks.mqtt_lan);
  $('serverMqttPublicDot').className=dotClass(!!checks.mqtt_public?.ok,!!checks.mqtt_public);
  $('serverHttpLocal').textContent=(server.local_http||'-').replace(/^https?:\/\//,'');
  $('serverHttpPublic').textContent=(server.public_http||'not configured').replace(/^https?:\/\//,'');
  $('serverMqttLocal').textContent=(server.local_mqtt_host||'-')+':'+(server.mqtt_port||1883);
  $('serverMqttPublic').textContent=server.public_mqtt_host?(server.public_mqtt_host+':'+(server.public_mqtt_port||server.mqtt_port||1883)):'not configured';
  setPulse('serverHttpLocalPulse',checks.http_lan);
  setPulse('serverHttpPublicPulse',checks.http_public);
  setPulse('serverMqttLocalPulse',checks.mqtt_lan);
  setPulse('serverMqttPublicPulse',checks.mqtt_public);
  const statusRow=([k,v])=>'<div class="status-item"><span>'+k+'</span><strong>'+v+'</strong></div>';
  const wifiIp=(alive.wifi&&alive.wifi.ip)||st.ip||'-';
  const lteIp=(alive.lte&&alive.lte.ip)||st.lte_ip||st.cellular_ip||'-';
  const checkBadges=(mqtt,http)=>badge(!!mqtt,'MQTT')+' '+badge(!!http,'HTTP');
  const gpsLat=Number(gps.lat||0), gpsLon=Number(gps.lon||0);
  const gpsPosition=(gps.has_fix||gps.valid)?(gpsLat.toFixed(6)+', '+gpsLon.toFixed(6)):'-';
  $('deviceConnectivity').innerHTML=[
    ['Active link',activeLink.toUpperCase()],
    ['WiFi',wifiOk?badge(true,'connected')+' '+wifiIp:badge(false,'offline')+' '+wifiIp],
    ['WiFi checks',checkBadges(wifiMqttOk,wifiHttpOk)],
    ['LTE',lteOk?badge(true,'connected')+' '+lteIp:badge(false,'offline')+' '+lteIp],
    ['LTE checks',checkBadges(lteMqttOk,lteHttpOk)]
  ].map(statusRow).join('');
  $('deviceSystem').innerHTML=[
    ['Transport',rdm.transport||st.mqtt_transport||'-'],
    ['Control MQTT',mqttOk?badge(true,'connected'):badge(false,'offline')],
    ['Sleep',sleepState==='sleeping'?badge(false,'sleeping'):badge(true,sleepState)],
    ['Firmware',rdm.fw_version||ota.fw_version||'-'],
    ['Uptime',fmtUptime(rdm.uptime_ms||st.uptime_ms||alive.uptime_ms)],
    ['SPIFFS',fmtBytes(fs.used)+' / '+fmtBytes(fs.total)]
  ].map(statusRow).join('');
  $('deviceGpsSummary').innerHTML=[
    ['Fix',gps.has_fix?badge(true,'fix'):badge(false,'no fix')],
    ['Position',gpsPosition],
    ['Speed',(Number(gps.speed_mps||0)*3.6).toFixed(1)+' km/h'],
    ['Satellites',gps.sat_total??'-'],
    ['UTC',gps.utc_valid?(gps.utc||'-'):'-'],
    ['Age',gps.age_ms!=null&&gps.age_ms>=0?fmtUptime(gps.age_ms):'-']
  ].map(statusRow).join('');
  renderVehicleMap(gps);
  const consoleMsgs=(state.messages||[]).filter(m=>m.topic.endsWith('/console/out'));
  $('consoleOut').textContent=localConsole+consoleMsgs.map(m=>m.payload.payload||'').join('');
  const final=consoleMsgs.slice().reverse().find(m=>m.payload.final);$('consoleBadge').textContent=final?'exit '+final.payload.exit_code:'idle';
  renderFiles();
  $('otaSummary').innerHTML=[['Firmware',ota.fw_version||rdm.fw_version||'-'],['GitHub OTA',ota.capabilities&&ota.capabilities.github_latest?badge(true,'available'):'-'],['URL OTA',ota.capabilities&&ota.capabilities.ota_url?badge(true,'available'):'-']].map(([k,v])=>'<div class="status-item"><span>'+k+'</span><strong>'+v+'</strong></div>').join('');
  $('otaState').textContent=JSON.stringify({state:ota,results:(state.messages||[]).filter(m=>m.topic.endsWith('/ota/result')).slice(-8)},null,2);
  $('messages').textContent=JSON.stringify((state.messages||[]).slice(-80),null,2);
}
function commandMatches(text){
  const q=(text||'').trim().toLowerCase();
  if(!q)return consoleCommands;
  return consoleCommands.filter(([cmd])=>cmd.toLowerCase().startsWith(q)||cmd.split(' ')[0].toLowerCase().startsWith(q));
}
function renderCommandSuggestions(matches){
  const box=$('commandSuggestions');
  if(!box)return;
  commandSuggestionIndex=Math.max(-1,Math.min(commandSuggestionIndex,matches.length-1));
  box.innerHTML=matches.map(([cmd,desc],i)=>'<button type="button" class="command-suggestion '+(i===commandSuggestionIndex?'active':'')+'" onmousedown="pickCommandSuggestion('+i+')"><code>'+escapeHtml(cmd)+'</code><span>'+escapeHtml(desc)+'</span></button>').join('');
  box.classList.toggle('show',matches.length>0&&document.activeElement===$('cmd'));
}
function showCommandSuggestions(){
  commandSuggestionIndex=-1;
  renderCommandSuggestions(commandMatches($('cmd').value));
}
function hideCommandSuggestions(){let box=$('commandSuggestions');if(box)box.classList.remove('show');commandSuggestionIndex=-1}
function pickCommandSuggestion(index){
  const matches=commandMatches($('cmd').value);
  if(index<0||index>=matches.length)return;
  $('cmd').value=matches[index][0];
  $('cmd').focus();
  hideCommandSuggestions();
}
function handleCommandKey(event){
  const matches=commandMatches($('cmd').value);
  if(event.key==='ArrowDown'){
    event.preventDefault();
    commandSuggestionIndex=(commandSuggestionIndex+1)%Math.max(1,matches.length);
    renderCommandSuggestions(matches);
  }else if(event.key==='ArrowUp'){
    event.preventDefault();
    commandSuggestionIndex=(commandSuggestionIndex<=0?matches.length:commandSuggestionIndex)-1;
    renderCommandSuggestions(matches);
  }else if(event.key==='Tab'&&matches.length){
    event.preventDefault();
    pickCommandSuggestion(commandSuggestionIndex>=0?commandSuggestionIndex:0);
  }else if(event.key==='Escape'){
    hideCommandSuggestions();
  }else if(event.key==='Enter'){
    event.preventDefault();
    sendCmd();
  }
}
async function sendCmd(){let c=$('cmd'), command=c.value.trim();if(!command)return;hideCommandSuggestions();localConsole+='> '+command+'\n';c.value='';let res=await api('/api/console',{command});showToast('Console command queued: '+res.command_id)}
function clearConsole(){localConsole='';$('consoleOut').textContent=''}
async function listFiles(silent=false){
  if(fileListBusy)return;
  fileListBusy=true; fileListRequestedAt=Date.now(); renderFiles();
  try{let r=await api('/api/files/list',{}); if(!silent)showToast('File list requested'); setTimeout(()=>{fileListBusy=false;renderFiles()},1600); return r}
  catch(e){fileListBusy=false;renderFiles(); if(!silent)showToast(e.message)}
}
async function deleteFile(path){path=path||'';if(!path)return;hideFileMenu();let r=await api('/api/files/delete',{path});showToast('Delete queued');setTimeout(()=>listFiles(true),1200);return r}
async function downloadFile(path){path=path||'';if(!path)return;hideFileMenu();let r=await api('/api/files/download',{path});showToast('Download queued');return r}
function uploadFile(file,path){
  return new Promise((resolve,reject)=>{
    const target=path||('/'+file.name), xhr=new XMLHttpRequest();
    uploadItems[target]={path:target,size:file.size,progress:1}; renderFiles();
    xhr.upload.onprogress=e=>{if(e.lengthComputable){uploadItems[target].progress=(e.loaded/e.total)*100;renderFiles()}};
    xhr.onload=()=>{try{let data=JSON.parse(xhr.responseText||'{}');if(xhr.status>=200&&xhr.status<300&&!data.error){uploadItems[target].progress=100;renderFiles();setTimeout(()=>{delete uploadItems[target];renderFiles();listFiles(true)},900);showToast('Upload queued: '+target);resolve(data)}else{throw new Error(data.error||xhr.statusText)}}catch(err){delete uploadItems[target];renderFiles();reject(err)}};
    xhr.onerror=()=>{delete uploadItems[target];renderFiles();reject(new Error('upload failed'))};
    xhr.open('POST','/api/files/upload?path='+encodeURIComponent(target));
    xhr.send(file);
  })
}
async function uploadPickedFiles(){let files=Array.from($('filePick').files||[]);if(!files.length)return;for(let f of files)await uploadFile(f,'/'+f.name);$('filePick').value=''}
function openFileMenu(event,path){event.preventDefault();fileMenuOpenedAt=Date.now();selectedFilePath=path;let m=$('fileMenu');m.style.left=Math.min(event.clientX,window.innerWidth-210)+'px';m.style.top=Math.min(event.clientY,window.innerHeight-130)+'px';m.classList.add('show')}
function hideFileMenu(){let m=$('fileMenu');if(m)m.classList.remove('show')}
function startFileLongPress(event,path){clearFileLongPress();if(event.pointerType==='mouse'&&event.button!==0)return;longPressTimer=setTimeout(()=>openFileMenu(event,path),520)}
function clearFileLongPress(){if(longPressTimer){clearTimeout(longPressTimer);longPressTimer=null}}
function downloadSelectedFile(){downloadFile(selectedFilePath)}
function deleteSelectedFile(){deleteFile(selectedFilePath)}
async function copySelectedFilePath(){if(!selectedFilePath)return;hideFileMenu();try{await navigator.clipboard.writeText(selectedFilePath);showToast('Path copied')}catch(e){showToast(selectedFilePath)}}
async function githubOta(){let r=await api('/api/ota/github',{});showToast('GitHub OTA queued: '+r.job_id)}
async function uploadBin(){let f=$('bin').files[0];if(!f)return;let r=await fetch('/api/ota/upload-bin?name='+encodeURIComponent(f.name),{method:'POST',body:await f.arrayBuffer()});let data=await r.json();showToast('OTA queued: '+data.job_id+' CRC '+data.crc32)}
function publishStateHint(){api('/api/console',{command:'mqtt status'}).then(r=>showToast('State command queued: '+r.command_id)).catch(e=>showToast(e.message))}
async function requestAlive(forceFull=false){try{const now=Date.now(), full=forceFull||now-lastAliveFullAt>60000;if(full)lastAliveFullAt=now;await api('/api/device/alive',{request_id:'a'+now,compact:!full})}catch(e){}}
async function setDeviceTransport(mode){
  const aliveMsg=latestMessage('rdm/alive'), alive=aliveMsg?aliveMsg.payload:{};
  if(mode==='lte' && !(alive.lte&&alive.lte.mqtt)){
    showToast('LTE switch blocked: MQTT is not reachable over LTE.');
    return;
  }
  pendingTransport=mode; pendingTransportUntil=Date.now()+15000; render();
  try{let r=await api('/api/device/transport',{mode});showToast('Transport switch queued: '+mode+' '+r.job_id); setTimeout(refresh,400)}
  catch(e){pendingTransport=null; render(); showToast(e.message)}
}
document.addEventListener('click',e=>{if(Date.now()-fileMenuOpenedAt<180)return;if(!$('fileMenu').contains(e.target))hideFileMenu()});
document.addEventListener('keydown',e=>{if(e.key==='Escape')hideFileMenu()});
drop.ondragover=e=>{e.preventDefault();drop.classList.add('drag')};drop.ondragleave=()=>drop.classList.remove('drag');drop.ondrop=async e=>{e.preventDefault();drop.classList.remove('drag');for(let f of e.dataTransfer.files)await uploadFile(f,'/'+f.name);setTimeout(()=>listFiles(true),500)}
setInterval(refresh,1000);setInterval(requestAlive,10000);setInterval(()=>{if(activeView==='files')listFiles(true)},10000);refresh().then(()=>{requestAlive(true);listFiles(true)}).catch(e=>showToast(e.message));
</script>
</body>
</html>"""


class Handler(BaseHTTPRequestHandler):
    state: AppState

    def log_message(self, fmt: str, *args: object) -> None:
        return

    def _send(self, code: int, data: bytes, content_type: str = "application/json") -> None:
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _json(self, value: object, code: int = 200) -> None:
        self._send(code, json.dumps(value, separators=(",", ":")).encode())

    def _body(self) -> bytes:
        return self.rfile.read(int(self.headers.get("Content-Length", "0")))

    def do_GET(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/":
            self._send(200, HTML.encode(), "text/html; charset=utf-8")
            return
        if parsed.path == "/api/state":
            query = urllib.parse.parse_qs(parsed.query)
            checks = {} if query.get("server_checks", ["1"])[0] == "0" else self.state.get_server_checks()
            with self.state.lock:
                self._json({
                    "retained": self.state.retained,
                    "messages": self.state.messages,
                    "topic_prefix": self.state.args.topic_prefix,
                    "server": {
                        "local_http": self.state.args.local_http_url,
                        "public_http": self.state.args.public_http_url,
                        "mqtt_host": self.state.args.mqtt_host,
                        "mqtt_port": self.state.args.mqtt_port,
                        "local_mqtt_host": self.state.args.local_mqtt_host,
                        "public_mqtt_host": self.state.args.public_mqtt_host,
                        "public_mqtt_port": self.state.args.public_mqtt_port,
                    },
                    "server_checks": checks,
                    "transfer_stats": self.state.transfer_stats,
                })
            return
        if parsed.path.startswith("/assets/"):
            file_path = self.state.assets / parsed.path.removeprefix("/assets/")
            if not file_path.is_file() or self.state.assets not in file_path.resolve().parents:
                self._json({"error": "not_found"}, 404)
                return
            content_type = "image/svg+xml" if file_path.suffix == ".svg" else "application/octet-stream"
            self._send(200, file_path.read_bytes(), content_type)
            return
        if parsed.path.startswith("/files/"):
            file_path = self.state.hosted / parsed.path.removeprefix("/files/")
            if not file_path.is_file() or self.state.hosted not in file_path.resolve().parents:
                self._json({"error": "not_found"}, 404)
                return
            data = file_path.read_bytes()
            self.state.add_transfer(self.state.infer_link(), "http", "down", len(data))
            self._send(200, data, "application/octet-stream")
            return
        self._json({"error": "not_found"}, 404)

    def do_PUT(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        if not parsed.path.startswith("/uploads/"):
            self._json({"error": "not_found"}, 404)
            return
        target = self.state.uploads / parsed.path.removeprefix("/uploads/")
        target.parent.mkdir(parents=True, exist_ok=True)
        data = self._body()
        target.write_bytes(data)
        self.state.add_transfer(self.state.infer_link(), "http", "up", len(data))
        self._json({"ok": True, "path": str(target)})

    def do_POST(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        if parsed.path == "/api/console":
            data = json.loads(self._body() or b"{}")
            payload = self.state.publish_job("console/cmd", {
                "session_id": "webui",
                "command_id": f"cmd-{uuid.uuid4().hex[:8]}",
                "command": data.get("command", ""),
            })
            self._json({"ok": True, "job_id": payload["job_id"], "command_id": payload["command_id"]})
        elif parsed.path == "/api/files/list":
            payload = self.state.publish_job("fs/jobs", {"op": "list"})
            self._json({"ok": True, "job_id": payload["job_id"]})
        elif parsed.path == "/api/files/delete":
            data = json.loads(self._body() or b"{}")
            payload = self.state.publish_job("fs/jobs", {"op": "delete", "path": data.get("path", "")})
            self._json({"ok": True, "job_id": payload["job_id"]})
        elif parsed.path == "/api/files/download":
            data = json.loads(self._body() or b"{}")
            job_id = f"job-{uuid.uuid4().hex[:12]}"
            self.state.publish_job("fs/jobs", {
                "job_id": job_id,
                "op": "get_url",
                "path": data.get("path", ""),
                "url": f"{self.state.args.public_base_url.rstrip('/')}/uploads/{job_id}",
            })
            self._json({"ok": True, "job_id": job_id})
        elif parsed.path == "/api/files/upload":
            name = query.get("path", ["/upload.bin"])[0].lstrip("/")
            job_id = f"job-{uuid.uuid4().hex[:12]}"
            target = self.state.hosted / job_id / Path(name).name
            target.parent.mkdir(parents=True, exist_ok=True)
            data = self._body()
            target.write_bytes(data)
            crc = zlib.crc32(data) & 0xFFFFFFFF
            self.state.publish_job("fs/jobs", {
                "job_id": job_id,
                "op": "put_url",
                "path": "/" + name,
                "url": f"{self.state.args.public_base_url.rstrip('/')}/files/{job_id}/{target.name}",
                "crc32": f"{crc:08x}",
            })
            self._json({"ok": True, "job_id": job_id, "crc32": f"{crc:08x}"})
        elif parsed.path == "/api/ota/github":
            payload = self.state.publish_job("ota/jobs", {"op": "github_latest"})
            self._json({"ok": True, "job_id": payload["job_id"]})
        elif parsed.path == "/api/ota/upload-bin":
            name = Path(query.get("name", ["firmware.bin"])[0]).name
            job_id = f"job-{uuid.uuid4().hex[:12]}"
            target = self.state.hosted / job_id / name
            target.parent.mkdir(parents=True, exist_ok=True)
            data = self._body()
            target.write_bytes(data)
            crc = zlib.crc32(data) & 0xFFFFFFFF
            self.state.publish_job("ota/jobs", {
                "job_id": job_id,
                "op": "ota_url",
                "url": f"{self.state.args.public_base_url.rstrip('/')}/files/{job_id}/{name}",
                "size": len(data),
                "crc32": f"{crc:08x}",
            })
            self._json({"ok": True, "job_id": job_id, "crc32": f"{crc:08x}"})
        elif parsed.path == "/api/device/alive":
            data = json.loads(self._body() or b"{}")
            request_id = str(data.get("request_id", f"a{uuid.uuid4().hex[:8]}"))
            if data.get("compact") is True:
                payload = self.state.publish_control("rdm/alive/request", {
                    "request_id": request_id,
                    "compact": True,
                    "local_http": self.state.args.local_http_url,
                    "public_http": self.state.args.public_http_url,
                    "local_mqtt_host": self.state.args.local_mqtt_host,
                    "public_mqtt_host": self.state.args.public_mqtt_host,
                    "mqtt_port": self.state.args.mqtt_port,
                    "public_mqtt_port": self.state.args.public_mqtt_port,
                })
                self._json({"ok": True, "request_id": payload["request_id"], "compact": True})
            else:
                payload = self.state.publish_job("rdm/alive/request", {
                    "request_id": request_id,
                    "local_http": self.state.args.local_http_url,
                    "public_http": self.state.args.public_http_url,
                    "local_mqtt_host": self.state.args.local_mqtt_host,
                    "public_mqtt_host": self.state.args.public_mqtt_host,
                    "mqtt_port": self.state.args.mqtt_port,
                    "public_mqtt_port": self.state.args.public_mqtt_port,
                })
                self._json({"ok": True, "job_id": payload["job_id"], "request_id": payload["request_id"], "compact": False})
        elif parsed.path == "/api/device/transport":
            data = json.loads(self._body() or b"{}")
            mode = str(data.get("mode", "wifi")).lower()
            force = bool(data.get("force", False))
            if mode in ("lte", "cellular") and not force:
                alive = self.state.latest_payload("rdm/alive")
                lte = alive.get("lte", {}) if isinstance(alive, dict) else {}
                if lte.get("mqtt") is not True:
                    self._json({
                        "error": "lte_mqtt_unreachable",
                        "detail": "LTE data may be up, but MQTT is not reachable over LTE. Switch blocked to keep control.",
                        "lte": lte,
                    }, 409)
                    return
            payload = self.state.publish_job("rdm/transport/set", {
                "mode": mode,
            })
            self._json({"ok": True, "job_id": payload["job_id"], "mode": payload["mode"]})
        else:
            self._json({"error": "not_found"}, 404)


def mqtt_loop(state: AppState) -> None:
    while True:
        try:
            state.mqtt.connect()
            state.mqtt.subscribe(f"{state.args.topic_prefix}/#")
            last_ping = time.monotonic()
            while True:
                if time.monotonic() - last_ping > 15:
                    state.mqtt.ping()
                    last_ping = time.monotonic()
                msg = state.mqtt.read()
                if msg:
                    state.record(*msg)
        except OSError as exc:
            print(f"MQTT reconnect after error: {exc}", flush=True)
            time.sleep(3)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="RemoteDeviceManager WebUI and MQTT controller")
    parser.add_argument("--mqtt-host", required=True)
    parser.add_argument("--mqtt-port", type=int, default=1883)
    parser.add_argument("--mqtt-user")
    parser.add_argument("--mqtt-password")
    parser.add_argument("--device", default="eboxster")
    parser.add_argument("--topic-prefix", default="eboxster")
    parser.add_argument("--http-host", default="0.0.0.0")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--public-base-url", required=True)
    parser.add_argument("--local-http-url", default="")
    parser.add_argument("--public-http-url", default="")
    parser.add_argument("--local-mqtt-host", default="")
    parser.add_argument("--public-mqtt-host", default="")
    parser.add_argument("--public-mqtt-port", type=int, default=0)
    parser.add_argument("--open", action="store_true")
    args = parser.parse_args()
    if not args.local_http_url:
        args.local_http_url = args.public_base_url
    lan_host = host_from_url(args.local_http_url)
    duckdns_host = read_duckdns_domain()
    if not args.local_mqtt_host:
        args.local_mqtt_host = lan_host or args.mqtt_host
    if not args.public_mqtt_host:
        args.public_mqtt_host = duckdns_host
    if not args.public_mqtt_port:
        args.public_mqtt_port = args.http_port if args.public_mqtt_host else args.mqtt_port
    if not args.public_http_url and duckdns_host:
        args.public_http_url = f"http://{duckdns_host}:{args.http_port}"
    return args


def main() -> int:
    args = parse_args()
    root = Path(__file__).resolve().parent / "work"
    assets = Path(__file__).resolve().parent / "assets"
    hosted = root / "hosted"
    uploads = root / "uploads"
    hosted.mkdir(parents=True, exist_ok=True)
    uploads.mkdir(parents=True, exist_ok=True)
    mqtt = MqttClient(args.mqtt_host, args.mqtt_port, f"rdm-webui-{uuid.uuid4().hex[:8]}", args.mqtt_user, args.mqtt_password)
    state = AppState(args=args, mqtt=mqtt, root=root, hosted=hosted, uploads=uploads, assets=assets)
    Handler.state = state
    threading.Thread(target=mqtt_loop, args=(state,), daemon=True).start()
    server_class = DualStackMuxingThreadingHTTPServer if ":" in args.http_host else MuxingThreadingHTTPServer
    server = server_class((args.http_host, args.http_port), Handler)
    server.mqtt_upstream_host = args.mqtt_host
    server.mqtt_upstream_port = args.mqtt_port
    url = f"http://127.0.0.1:{args.http_port}/"
    print(f"RemoteDeviceManager WebUI: {url}")
    print(f"Device HTTP base URL: {args.public_base_url}")
    print(f"Public MQTT endpoint: {args.public_mqtt_host}:{args.public_mqtt_port} -> {args.mqtt_host}:{args.mqtt_port}")
    if args.open:
        webbrowser.open(url)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
