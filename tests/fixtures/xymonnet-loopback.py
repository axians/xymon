#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import os
import socket
import ssl
import struct
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
        if self.path == "/missing":
            self.send_fixture(404, b"not found\n")
        else:
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


def serve_ftps(listener, context):
    while True:
        connection, _ = listener.accept()
        try:
            with context.wrap_socket(connection, server_side=True) as tls_connection:
                tls_connection.sendall(b"220 xymon TLS fixture\r\n")
                tls_connection.recv(1024)
        except ssl.SSLError:
            connection.close()


def serve_dns(dns_socket):
    while True:
        request, client = dns_socket.recvfrom(4096)
        try:
            offset = 12
            while request[offset] != 0:
                offset += request[offset] + 1
            question_end = offset + 5
            query_type = struct.unpack("!H", request[question_end - 4:question_end - 2])[0]
            query_flags = struct.unpack("!H", request[2:4])[0]
            response_flags = 0x8400 | (query_flags & 0x0100)
            answer_count = 1 if query_type == 1 else 0
            if answer_count == 0:
                response_flags |= 3
            response = struct.pack(
                "!HHHHHH",
                struct.unpack("!H", request[:2])[0],
                response_flags,
                1,
                answer_count,
                0,
                0,
            ) + request[12:question_end]
            if answer_count:
                response += (
                    b"\xc0\x0c"
                    + struct.pack("!HHIH", 1, 1, 60, 4)
                    + socket.inet_aton("127.0.0.1")
                )
            dns_socket.sendto(response, client)
        except (IndexError, struct.error):
            continue


def main():
    if len(sys.argv) not in (2, 4):
        raise SystemExit("usage: xymonnet-loopback.py READYFILE [CERT KEY]")

    ssh_listener = socket.socket()
    ssh_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ssh_listener.bind(("127.0.0.1", 0))
    ssh_listener.listen()

    tls_listener = None
    tls_context = None
    if len(sys.argv) == 4:
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(sys.argv[2], sys.argv[3])
        tls_listener = socket.socket()
        tls_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        tls_listener.bind(("127.0.0.1", 0))
        tls_listener.listen()

    dns_socket = None
    if os.environ.get("XYMONNET_DNS_FIXTURE") == "1":
        dns_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        dns_socket.bind(("127.0.0.1", 53))

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(sys.argv[1], "w", encoding="ascii") as ready:
        tls_port = tls_listener.getsockname()[1] if tls_listener else ""
        dns_ready = "1" if dns_socket else ""
        ready.write(
            f"{httpd.server_port} {ssh_listener.getsockname()[1]} "
            f"{tls_port} {dns_ready}\n"
        )

    threading.Thread(target=serve_ssh, args=(ssh_listener,), daemon=True).start()
    if tls_listener and tls_context:
        threading.Thread(
            target=serve_ftps, args=(tls_listener, tls_context), daemon=True
        ).start()
    if dns_socket:
        threading.Thread(target=serve_dns, args=(dns_socket,), daemon=True).start()
    httpd.serve_forever()


if __name__ == "__main__":
    main()