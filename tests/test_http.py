#!/usr/bin/env python3
"""Loopback integration checks for the small HTTP server and process shutdown."""

import ctypes
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
APP = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build" / "opiz3-gateway"
STUBS = r"""
#include <stdarg.h>
static int led;
int gpio_dev_get_led(void) { return led; }
int gpio_dev_set_led(int value) { led = value; return 0; }
void log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
"""


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def raw_request(port, chunks, delays=()):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        sock.settimeout(5)
        for index, chunk in enumerate(chunks):
            sock.sendall(chunk)
            if index < len(delays):
                time.sleep(delays[index])
        response = bytearray()
        while True:
            part = sock.recv(8192)
            if not part:
                break
            response.extend(part)
    return bytes(response).split(b"\r\n\r\n", 1)


def request(port, method, path, token=None, host=b"localhost", extra_headers=b""):
    auth = b"" if token is None else b"Authorization: Bearer " + token + b"\r\n"
    host_header = b"" if host is None else b"Host: " + host + b"\r\n"
    head = (method + b" " + path + b" HTTP/1.1\r\n" + host_header +
            auth + extra_headers + b"\r\n")
    return raw_request(port, [head])


def check_http_routes(tmpdir):
    stubs_path = Path(tmpdir) / "http_stubs.c"
    library_path = Path(tmpdir) / "libhttp_test.so"
    stubs_path.write_text(STUBS)
    subprocess.run(
        ["gcc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-std=gnu11",
         "-pthread", "-D_GNU_SOURCE", "-I", str(ROOT / "src"),
         str(ROOT / "src/http_server.c"), str(ROOT / "src/shared.c"),
         str(stubs_path), "-o", str(library_path)],
        check=True,
    )
    lib = ctypes.CDLL(str(library_path))
    lib.http_server_start.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p]
    lib.http_server_start.restype = ctypes.c_int
    lib.shared_set_serial_line.argtypes = [ctypes.c_char_p]
    lib.shared_set_running.argtypes = [ctypes.c_int]
    lib.gpio_dev_get_led.restype = ctypes.c_int
    lib.shared_init()
    port = free_port()
    assert lib.http_server_start(port, b"0.0.0.0", b"") == -1
    assert lib.http_server_start(port, b"127.0.0.1", b"secret") == 0

    try:
        head, _ = request(port, b"GET", b"/api/status")
        assert b"401 Unauthorized" in head and b"WWW-Authenticate: Bearer" in head
        head, _ = request(port, b"GET", b"/api/status", b"wrong")
        assert b"401 Unauthorized" in head
        head, body = request(port, b"GET", b"/api/status", b"secret")
        assert b"200 OK" in head and isinstance(json.loads(body), dict)

        head, _ = request(port, b"POST", b"/api/status", b"secret")
        assert b"405 Method Not Allowed" in head and b"Allow: GET" in head
        head, _ = request(port, b"GET", b"/api/gpio?state=on", b"secret")
        assert b"405 Method Not Allowed" in head and b"Allow: POST" in head
        assert lib.gpio_dev_get_led() == 0
        head, _ = request(port, b"POST", b"/api/gpio?state=on", b"secret")
        assert b"403 Forbidden" in head and lib.gpio_dev_get_led() == 0
        head, body = request(
            port, b"POST", b"/api/gpio?state=on", b"secret",
            extra_headers=b"X-Requested-With: XMLHttpRequest\r\n",
        )
        assert b"200 OK" in head and json.loads(body)["led"] == 1
        head, _ = request(port, b"GET", b"/api/status", b"secret", host=b"other.example")
        assert b"200 OK" in head  # Token mode keeps its existing Host behavior.

        lib.shared_set_serial_line(b"a\tb\rc" + b'"' * 400)
        for path, key in ((b"/api/serial", "line"), (b"/api/status", "serial_line")):
            head, body = request(port, b"GET", path, b"secret")
            assert b"200 OK" in head
            assert json.loads(body)[key] == "a\tb\rc" + '"' * 400

        head, _ = raw_request(
            port,
            [b"GET /api/sta", b"tus HTTP/1.1\r\nHost: localhost\r\n"
             b"Authorization: Bearer secret\r\n\r\n"],
            [0.1],
        )
        assert b"200 OK" in head
        head, _ = raw_request(port, [b"GET /api/status HTTP/1.1\r\nX: " + b"a" * 2050 + b"\r\n\r\n"])
        assert b"431 Request Header Fields Too Large" in head
        head, _ = raw_request(port, [b"GET /api/status HTTP/1.1\r\nHost: localhost\r\n"])
        assert b"408 Request Timeout" in head

        idle = socket.create_connection(("127.0.0.1", port), timeout=5)
        try:
            time.sleep(0.1)
            start = time.monotonic()
            lib.shared_set_running(0)
            lib.http_server_stop()
            assert time.monotonic() - start < 1.0
        finally:
            idle.close()
    finally:
        lib.shared_set_running(0)
        lib.http_server_stop()
        lib.shared_destroy()


def wait_until_listening(port, proc):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError(f"gateway exited before listening: {proc.returncode}")
        try:
            request(port, b"GET", b"/")
            return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("gateway did not start listening")


def check_real_process_signals():
    if not APP.is_file():
        raise AssertionError(f"build the gateway first: {APP}")
    port = free_port()
    proc = subprocess.Popen(
        [str(APP), "--simulate", "--port", str(port)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    idle = None
    try:
        wait_until_listening(port, proc)
        head, _ = request(port, b"GET", b"/api/status")
        assert b"200 OK" in head
        head, _ = request(port, b"GET", b"/api/status", host=None)
        assert b"400 Bad Request" in head
        head, _ = request(port, b"GET", b"/api/status", host=b"attacker.example")
        assert b"403 Forbidden" in head
        head, _ = request(port, b"GET", b"/api/status", host=f"localhost:{port}".encode())
        assert b"200 OK" in head
        head, _ = request(port, b"GET", b"/api/status", host=b"localhost:1")
        assert b"403 Forbidden" in head
        head, _ = request(port, b"POST", b"/api/gpio?state=on")
        assert b"403 Forbidden" in head
        head, _ = request(
            port, b"OPTIONS", b"/api/gpio?state=on",
            extra_headers=(b"Origin: http://attacker.example\r\n"
                           b"Access-Control-Request-Method: POST\r\n"
                           b"Access-Control-Request-Headers: X-Requested-With\r\n"),
        )
        assert b"405 Method Not Allowed" in head
        assert b"Access-Control-Allow-Origin" not in head
        head, body = request(
            port, b"POST", b"/api/gpio?state=on",
            extra_headers=b"X-Requested-With: XMLHttpRequest\r\n",
        )
        assert b"200 OK" in head and json.loads(body)["led"] == 1

        proc.send_signal(signal.SIGHUP)
        time.sleep(0.2)
        assert proc.poll() is None, "SIGHUP should leave the gateway running"
        head, _ = request(port, b"GET", b"/")
        assert b"200 OK" in head

        idle = socket.create_connection(("127.0.0.1", port), timeout=5)
        time.sleep(0.1)
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=8) == 0, "gateway failed to stop after SIGTERM"
    finally:
        if idle is not None:
            idle.close()
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def main():
    with tempfile.TemporaryDirectory(prefix="opiz3-http-test-") as tmpdir:
        check_http_routes(tmpdir)
    check_real_process_signals()
    print("HTTP loopback tests passed (SIGHUP stays available; SIGTERM stops with an idle client)")


if __name__ == "__main__":
    main()
