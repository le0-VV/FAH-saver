"""Exercise the Windows native source against disposable synthetic endpoints."""

import argparse
import base64
import hashlib
import json
import socket
import struct
import subprocess
import threading


def send(connection, value):
    body = json.dumps(value).encode()
    header = bytes((0x81, len(body))) if len(body) < 126 else bytes((0x81, 126)) + struct.pack("!H", len(body))
    connection.sendall(header + body)


def serve(connection, mode):
    with connection:
        connection.settimeout(5)
        try:
            request = b""
            while b"\r\n\r\n" not in request:
                part = connection.recv(4096)
                if not part:
                    return
                request += part
                if len(request) > 65536:
                    raise RuntimeError("Oversized test handshake")
            if mode == "handshake":
                while connection.recv(4096):
                    pass
                return
            key = next(line.split(":", 1)[1].strip() for line in request.decode().split("\r\n")
                       if line.lower().startswith("sec-websocket-key:"))
            accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
            connection.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n").encode())
            if mode == "frames":
                send(connection, {"units": [{"id": "synthetic", "state": "RUN"}]})
                if not connection.recv(4096):
                    return
                send(connection, ["viz", "synthetic", "topology",
                                  {"atoms": [["C", "SYN", 0, 0, 6]], "bonds": []}])
                send(connection, ["viz", "synthetic", "frames", 0, [[0, 0, 0]]])
            while connection.recv(4096):
                pass
        except (OSError, StopIteration):
            # Cancelling the test client intentionally aborts these connections.
            pass


def trial(executable, mode, delay, cycles):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    listener.settimeout(0.1)
    done = threading.Event()
    children = []

    def accept():
        while not done.is_set():
            try:
                connection, _ = listener.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            worker = threading.Thread(target=serve, args=(connection, mode))
            worker.start()
            children.append(worker)

    worker = threading.Thread(target=accept)
    worker.start()
    try:
        command = [executable, "--url", f"ws://127.0.0.1:{listener.getsockname()[1]}/api/websocket",
                   "--cycles", str(cycles), "--delay-ms", str(delay)]
        if mode == "frames":
            command.append("--expect-frame")
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"{mode}: {result.stderr.strip()}")
        metrics = json.loads(result.stdout)
        if metrics["cycles"] != cycles or metrics["late_callbacks"] or metrics["maximum_stop_ms"] > 1000:
            raise RuntimeError(f"{mode}: invalid cancellation result")
        print(json.dumps({"case": mode, "delay_ms": delay, **metrics}))
    finally:
        done.set()
        listener.close()
        worker.join()
        for child in children:
            child.join(6)
            if child.is_alive():
                raise RuntimeError("Synthetic server thread did not end")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="Path to the Windows cancellation-check executable")
    args = parser.parse_args()
    trial(args.executable, "handshake", 0, 10)
    trial(args.executable, "handshake", 100, 10)
    trial(args.executable, "idle-websocket", 300, 10)
    trial(args.executable, "frames", 500, 5)
