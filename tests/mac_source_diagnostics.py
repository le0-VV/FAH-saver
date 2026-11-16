"""Check native macOS WebSocket errors and optional authorised direct routes."""

import argparse
import base64
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit


def capture(executable, port, folder):
    output = Path(folder) / "absent-capture.json"
    result = subprocess.run([executable, "--url", f"ws://127.0.0.1:{port}/api/websocket",
                             "--output", str(output), "--seconds", "2", "--frames", "1"],
                            capture_output=True, text=True, timeout=6)
    if result.returncode == 0 or output.exists():
        raise RuntimeError("A rejected connection must not create a capture")
    return result.stderr


def rejected_upgrade(executable, status):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(5)
    failures = []

    def respond():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                request = b""
                while b"\r\n\r\n" not in request:
                    part = connection.recv(4096)
                    if not part:
                        raise RuntimeError("Client closed before sending its upgrade request")
                    request += part
                    if len(request) > 65536:
                        raise RuntimeError("Oversized test request")
                body = b"PRIVATE_RESPONSE_BODY"
                response = (f"HTTP/1.1 {status} Rejected\r\nContent-Length: {len(body)}\r\n"
                            "Connection: close\r\nX-Private-Test: PRIVATE_RESPONSE_HEADER\r\n\r\n").encode()
                connection.sendall(response + body)
        except Exception as error:
            failures.append(str(error))

    worker = threading.Thread(target=respond)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix="fah-mac-diagnostic-") as folder:
            diagnostic = capture(executable, listener.getsockname()[1], folder)
        if f"HTTP {status}" not in diagnostic or "URL error -1011" not in diagnostic:
            raise RuntimeError(f"Missing upgrade status/code: {diagnostic.strip()}")
        if "handshake failed" not in diagnostic or "proxy routing" not in diagnostic:
            raise RuntimeError("Missing actionable handshake context")
        if "PRIVATE_RESPONSE_" in diagnostic:
            raise RuntimeError("HTTP response data leaked into the diagnostic")
    finally:
        listener.close()
        worker.join(6)
    if worker.is_alive() or failures:
        raise RuntimeError(f"Synthetic server failed: {failures}")
    print(f"HTTP {status} status/code/context/privacy passed")


def refused_connection(executable):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="fah-mac-diagnostic-") as folder:
        diagnostic = capture(executable, port, folder)
    if "URL error -1004" not in diagnostic or "FAH listener" not in diagnostic:
        raise RuntimeError(f"Missing refused-connection context: {diagnostic.strip()}")
    if "HTTP " in diagnostic:
        raise RuntimeError("A refused TCP connection must not invent an HTTP status")
    print("Connection-refused code/context passed")


def lan_direct_connection(executable, host):
    address = ipaddress.ip_address(host)
    if not address.is_private or address.is_loopback:
        raise RuntimeError("--lan-host must be this Mac's private, non-loopback LAN address")
    proxy_before = subprocess.check_output(["scutil", "--proxy"])
    if not all(re.search(rb"\b" + kind + rb"Enable\s*:\s*1", proxy_before) for kind in (b"HTTP", b"HTTPS", b"SOCKS")):
        raise RuntimeError("LAN routing check requires the existing system HTTP/HTTPS/SOCKS proxies to be enabled")
    listener = socket.socket()
    listener.bind((host, 0))
    listener.listen(1)
    listener.settimeout(5)
    failures = []

    def send(connection, value):
        body = json.dumps(value).encode()
        if len(body) >= 126:
            raise RuntimeError("Test frame exceeds its small-fixture limit")
        connection.sendall(bytes((0x81, len(body))) + body)

    def respond():
        try:
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                request = b""
                while b"\r\n\r\n" not in request:
                    part = connection.recv(4096)
                    if not part:
                        raise RuntimeError("Client closed before upgrade")
                    request += part
                    if len(request) > 65536:
                        raise RuntimeError("Oversized test request")
                key = next(line.split(":", 1)[1].strip() for line in request.decode().split("\r\n")
                           if line.lower().startswith("sec-websocket-key:"))
                accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                connection.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                    "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
                send(connection, {"units": [{"id": "synthetic", "state": "RUN"}]})
                if not connection.recv(4096):
                    raise RuntimeError("Client did not request visualisation")
                send(connection, ["viz", "synthetic", "topology", {"atoms": [["C", "SYN", 0, 0, 6]], "bonds": []}])
                send(connection, ["viz", "synthetic", "frames", 0, [[0, 0, 0]]])
                connection.recv(4096)
        except Exception as error:
            failures.append(str(error))

    worker = threading.Thread(target=respond)
    worker.start()
    try:
        with tempfile.TemporaryDirectory(prefix="fah-mac-lan-") as folder:
            output = Path(folder) / "synthetic-capture.json"
            result = subprocess.run([executable, "--url", f"ws://{host}:{listener.getsockname()[1]}/api/websocket",
                                     "--output", str(output), "--seconds", "3", "--frames", "1"],
                                    capture_output=True, text=True, timeout=6)
            if result.returncode or not output.is_file():
                raise RuntimeError(f"Native LAN capture failed: {result.stderr.strip()}; server errors: {failures}")
            capture = json.loads(output.read_text())
            if set(capture) != {"topology", "frames"} or len(capture["frames"]) != 1:
                raise RuntimeError("Invalid synthetic LAN capture")
    finally:
        listener.close()
        worker.join(6)
    if worker.is_alive() or failures:
        raise RuntimeError(f"Synthetic LAN server failed: {failures}")
    if subprocess.check_output(["scutil", "--proxy"]) != proxy_before:
        raise RuntimeError("System proxy settings changed during the test")
    print("Native LAN WebSocket capture passed with system HTTP/HTTPS/SOCKS proxies enabled and unchanged")


def remote_direct_connection(executable, url):
    parts = urlsplit(url)
    address = ipaddress.ip_address(parts.hostname)
    if parts.scheme not in ("ws", "wss") or address.is_loopback or parts.username or parts.password:
        raise RuntimeError("--remote-source requires an authorised remote numeric-IP WebSocket URL")
    proxy_before = subprocess.check_output(["scutil", "--proxy"])
    if not all(re.search(rb"\b" + kind + rb"Enable\s*:\s*1", proxy_before) for kind in (b"HTTP", b"HTTPS", b"SOCKS")):
        raise RuntimeError("Remote routing regression requires existing system HTTP/HTTPS/SOCKS proxies to be enabled")
    with tempfile.TemporaryDirectory(prefix="fah-mac-remote-") as folder:
        output = Path(folder) / "capture.json"
        result = subprocess.run([executable, "--url", url, "--output", str(output), "--seconds", "10", "--frames", "2"],
                                capture_output=True, text=True, timeout=13)
        if result.returncode or not output.is_file():
            raise RuntimeError(f"Native remote numeric-IP capture failed: {result.stderr.strip()}")
        capture = json.loads(output.read_text())
        if set(capture) != {"topology", "frames"} or len(capture["frames"]) != 2:
            raise RuntimeError("Remote source did not supply two valid visualisation snapshots")
        atoms = len(capture["topology"]["atoms"])
        bonds = len(capture["topology"]["bonds"])
    if subprocess.check_output(["scutil", "--proxy"]) != proxy_before:
        raise RuntimeError("System proxy settings changed during the test")
    print(f"Native remote numeric-IP capture passed: {atoms} atoms, {bonds} bonds, 2 frames; system proxies unchanged")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="Path to the native fah_capture executable")
    parser.add_argument("--lan-host", help="This Mac's non-loopback LAN address, to test deliberate direct routing")
    parser.add_argument("--remote-source", action="append", default=[],
                        help="Explicitly authorised remote numeric-IP FAH URL; receives two snapshots and checks routing under enabled system proxies")
    args = parser.parse_args()
    rejected_upgrade(args.executable, 403)
    rejected_upgrade(args.executable, 503)
    refused_connection(args.executable)
    if args.lan_host:
        lan_direct_connection(args.executable, args.lan_host)
    for source in args.remote_source:
        remote_direct_connection(args.executable, source)
