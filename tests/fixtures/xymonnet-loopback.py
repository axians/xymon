#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def send_fixture(self, status=200, body=b"status=ok\n", content_type="text/plain"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.send_fixture()

    def do_GET(self):
        if self.path == "/missing":
            self.send_fixture(404, b"not found\n")
        elif self.path == "/json":
            self.send_fixture(body=b'{"status":"ok"}\n', content_type="application/json")
        else:
            self.send_fixture()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = self.rfile.read(length)
        if (self.path == "/soap" and
                self.headers.get_content_type() == "application/soap+xml" and
                request == b"<request/>"):
            self.send_fixture(body=b"soap-ok\n", content_type="application/soap+xml")
        elif self.path == "/soap":
            self.send_fixture(400, b"soap-fault\n", "application/soap+xml")
        else:
            self.send_fixture(body=b"received:" + request + b"\n")


def serve_ssh(listener):
    while True:
        connection, _ = listener.accept()
        with connection:
            connection.sendall(b"SSH-2.0-xymon-fixture\r\n")
            connection.recv(1024)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: xymonnet-loopback.py READYFILE")

    ssh_listener = socket.socket()
    ssh_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ssh_listener.bind(("127.0.0.1", 0))
    ssh_listener.listen()

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(sys.argv[1], "w", encoding="ascii") as ready:
        ready.write(f"{httpd.server_port} {ssh_listener.getsockname()[1]}\n")

    threading.Thread(target=serve_ssh, args=(ssh_listener,), daemon=True).start()
    httpd.serve_forever()


if __name__ == "__main__":
    main()