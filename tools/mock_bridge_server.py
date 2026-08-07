#!/usr/bin/env python3
"""Mock ESP32 MQTT bridge test server for Stikka-NG.

This helper simulates an ESP printer bridge:
- publishes retained status to /<printername>/status/
- subscribes to /<printername>/command/
- accepts ZPL and image payloads and optionally forwards them to a TCP
  printer target
- publishes job status updates (accepted/done/failed)

Topic layout and payload handling here mirror esp32/src/mqtt_bridge.cpp;
keep the two in sync.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import threading
import time
from dataclasses import dataclass

import paho.mqtt.client as mqtt


@dataclass
class Config:
    broker_host: str
    broker_port: int
    username: str
    password: str
    printer_name: str
    dpi: int
    label_width: int
    label_length: int
    forward_host: str
    forward_port: int
    fake_printer_host: str
    fake_printer_port: int


def status_topic(cfg: Config) -> str:
    return f"/{cfg.printer_name}/status/"


def command_topic(cfg: Config) -> str:
    return f"/{cfg.printer_name}/command/"


def publish_status(client: mqtt.Client, cfg: Config, phase: str, last_error: str = "") -> None:
    payload = {
        "printer_name": cfg.printer_name,
        "name": cfg.printer_name,
        "online": True,
        "busy": phase == "printing",
        "phase": phase,
        "type": "zpl",
        "dpi": cfg.dpi,
        "last_error": last_error,
        "label": {
            "width": cfg.label_width,
            "length": cfg.label_length,
            "isRound": False,
            "verticalOffset": 0,
            "cut": False,
        },
        "capabilities": {
            "type": "zpl",
            "dpi": cfg.dpi,
            "label": {
                "width": cfg.label_width,
                "length": cfg.label_length,
                "isRound": False,
                "verticalOffset": 0,
                "cut": False,
            },
        },
    }
    client.publish(status_topic(cfg), json.dumps(payload), qos=1, retain=True)


def publish_job_status(client: mqtt.Client, cfg: Config, job_id: str, status: str, message: str) -> None:
    payload = {
        "printer_name": cfg.printer_name,
        "job_id": job_id,
        "status": status,
        "message": message,
    }
    client.publish(status_topic(cfg), json.dumps(payload), qos=1, retain=False)


def send_to_tcp_target(host: str, port: int, data: str) -> None:
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.sendall(data.encode("utf-8"))


def send_bytes_to_tcp_target(host: str, port: int, data: bytes) -> None:
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.sendall(data)


def start_fake_printer(host: str, port: int) -> threading.Thread:
    def worker() -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((host, port))
            server.listen(5)
            print(f"[fake-printer] listening on {host}:{port}")
            while True:
                conn, addr = server.accept()
                with conn:
                    data = conn.recv(1024 * 1024)
                    print(f"[fake-printer] received {len(data)} bytes from {addr}")
                    if data.startswith(b"^XA"):
                        text = data.decode("utf-8", errors="replace")
                        print("[fake-printer] ---- ZPL START ----")
                        print(text)
                        print("[fake-printer] ---- ZPL END ----")
                    else:
                        print("[fake-printer] binary payload received")

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    return thread


def _resolve_zpl_body(payload: dict) -> str:
    encoding = str(payload.get("payload_encoding", ""))

    if encoding == "utf8":
        return str(payload.get("payload", ""))
    if encoding == "base64_utf8":
        return base64.b64decode(str(payload.get("payload", ""))).decode("utf-8")

    raise ValueError("payload_encoding must be utf8 or base64_utf8 for zpl")


def _resolve_image_bytes(payload: dict) -> bytes:
    encoding = str(payload.get("payload_encoding", ""))

    body = str(payload.get("payload", ""))
    if encoding == "data_url":
        if "," not in body:
            raise ValueError("invalid data_url payload")
        _, b64 = body.split(",", 1)
    elif encoding == "base64_png":
        b64 = body
    else:
        raise ValueError("unsupported image payload_encoding")
    return base64.b64decode(b64)


def build_client(cfg: Config) -> mqtt.Client:
    client = mqtt.Client(client_id=f"mock-bridge-{cfg.printer_name}-{os.getpid()}", protocol=mqtt.MQTTv311)
    if cfg.username:
        client.username_pw_set(cfg.username, cfg.password)

    def on_connect(_client: mqtt.Client, _userdata, _flags, rc: int) -> None:
        if rc != 0:
            print(f"[mqtt] connect failed rc={rc}")
            return
        print("[mqtt] connected")
        _client.subscribe(command_topic(cfg), qos=1)
        publish_status(_client, cfg, "ready")
        print(f"[mqtt] subscribed to {command_topic(cfg)}")

    def on_message(_client: mqtt.Client, _userdata, msg: mqtt.MQTTMessage) -> None:
        print(f"[mqtt] command received on {msg.topic} ({len(msg.payload)} bytes)")
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except Exception as exc:
            publish_job_status(_client, cfg, "", "failed", f"invalid JSON: {exc}")
            return

        job_id = str(payload.get("job_id", ""))
        payload_type = str(payload.get("payload_type", ""))

        publish_job_status(_client, cfg, job_id, "accepted", "job accepted")
        publish_status(_client, cfg, "printing")

        try:
            if payload_type == "zpl":
                zpl_text = _resolve_zpl_body(payload)
                if cfg.forward_host:
                    send_to_tcp_target(cfg.forward_host, cfg.forward_port, zpl_text)
                publish_job_status(_client, cfg, job_id, "done", "zpl sent")
            elif payload_type == "image":
                raw = _resolve_image_bytes(payload)
                if cfg.forward_host:
                    send_bytes_to_tcp_target(cfg.forward_host, cfg.forward_port, raw)
                publish_job_status(_client, cfg, job_id, "done", f"image bytes sent ({len(raw)} bytes)")
            else:
                raise ValueError("unsupported payload_type")
            publish_status(_client, cfg, "ready")
        except Exception as exc:
            publish_job_status(_client, cfg, job_id, "failed", f"send failed: {exc}")
            publish_status(_client, cfg, "error", str(exc))

    client.on_connect = on_connect
    client.on_message = on_message
    return client


def parse_args() -> Config:
    parser = argparse.ArgumentParser(description="Mock ESP32 MQTT bridge")
    parser.add_argument("--broker-host", default="127.0.0.1")
    parser.add_argument("--broker-port", type=int, default=1883)
    parser.add_argument("--username", default="")
    parser.add_argument("--password", default="")
    parser.add_argument("--printer-name", default="stikka-test")
    parser.add_argument("--dpi", type=int, default=203)
    parser.add_argument("--label-width", type=int, default=55)
    parser.add_argument("--label-length", type=int, default=55)
    parser.add_argument("--forward-host", default="")
    parser.add_argument("--forward-port", type=int, default=9100)
    parser.add_argument("--fake-printer-host", default="127.0.0.1")
    parser.add_argument("--fake-printer-port", type=int, default=9100)
    args = parser.parse_args()

    return Config(
        broker_host=args.broker_host,
        broker_port=args.broker_port,
        username=args.username,
        password=args.password,
        printer_name=args.printer_name,
        dpi=args.dpi,
        label_width=args.label_width,
        label_length=args.label_length,
        forward_host=args.forward_host,
        forward_port=args.forward_port,
        fake_printer_host=args.fake_printer_host,
        fake_printer_port=args.fake_printer_port,
    )


def main() -> None:
    cfg = parse_args()

    if not cfg.forward_host:
        print("[info] no --forward-host provided; using local fake TCP printer")
        start_fake_printer(cfg.fake_printer_host, cfg.fake_printer_port)
        cfg.forward_host = cfg.fake_printer_host
        cfg.forward_port = cfg.fake_printer_port

    print(f"[mqtt] broker={cfg.broker_host}:{cfg.broker_port}")
    print(f"[mqtt] command topic={command_topic(cfg)}")
    print(f"[mqtt] status topic={status_topic(cfg)}")

    client = build_client(cfg)
    client.connect(cfg.broker_host, cfg.broker_port, keepalive=30)
    client.loop_start()

    try:
        while True:
            time.sleep(15)
            publish_status(client, cfg, "ready")
    except KeyboardInterrupt:
        print("\n[exit] stopping mock bridge")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
