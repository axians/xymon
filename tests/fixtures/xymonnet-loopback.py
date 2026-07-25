#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import base64
import os
import socket
import ssl
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):  # pylint: disable=redefined-builtin
        pass

    def send_fixture(
        self, status=200, body=b"status=ok\n", content_type="text/plain"
    ):
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
            self.send_fixture(
                body=b'{"status":"ok"}\n', content_type="application/json"
            )
        elif self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/good")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/auth":
            credentials = base64.b64encode(b"fixture:password").decode("ascii")
            expected = "Basic " + credentials
            if self.headers.get("Authorization") == expected:
                self.send_fixture(body=b"authenticated\n")
            else:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="xymonnet"')
                self.send_header("Content-Length", "0")
                self.end_headers()
        else:
            self.send_fixture()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = self.rfile.read(length)
        if (self.path == "/soap" and
                self.headers.get_content_type() == "application/soap+xml" and
                request == b"<request/>"):
            self.send_fixture(
                body=b"soap-ok\n", content_type="application/soap+xml"
            )
        elif self.path == "/soap":
            self.send_fixture(400, b"soap-fault\n", "application/soap+xml")
        else:
            self.send_fixture(body=b"received:" + request + b"\n")


def serve_banner(listener, banner):
    while True:
        connection, _ = listener.accept()
        with connection:
            connection.sendall(banner)
            connection.recv(1024)


def serve_telnet(listener):
    while True:
        connection, _ = listener.accept()
        with connection:
            connection.settimeout(2)
            connection.sendall(b"\xff\xfb\x01")
            try:
                response = connection.recv(3)
            except socket.timeout:
                continue
            if response == b"\xff\xfe\x01":
                connection.sendall(b"xymonnet telnet login:\r\n")


def serve_ftps(listener, context):
    while True:
        connection, _ = listener.accept()
        try:
            with context.wrap_socket(
                connection, server_side=True
            ) as tls_connection:
                tls_connection.sendall(b"220 xymon TLS fixture\r\n")
                tls_connection.recv(1024)
        except ssl.SSLError:
            connection.close()


def serve_dns(dns_socket):
    while True:
        request, client = dns_socket.recvfrom(4096)
        try:
            offset = 12
            labels = []
            while request[offset] != 0:
                label_length = request[offset]
                offset += 1
                label = request[offset:offset + label_length].decode("ascii")
                labels.append(label)
                offset += label_length
            question_end = offset + 5
            query_type = struct.unpack(
                "!H", request[question_end - 4:question_end - 2]
            )[0]
            query_name = ".".join(labels)
            query_flags = struct.unpack("!H", request[2:4])[0]
            response_flags = 0x8400 | (query_flags & 0x0100)
            answer_count = int(
                query_type == 1 and query_name == "fixture.xymon.test"
            )
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
        except (IndexError, UnicodeDecodeError, struct.error):
            continue


def ntp_timestamp(timestamp):
    ntp_time = timestamp + 2208988800
    seconds = int(ntp_time)
    fraction = int((ntp_time - seconds) * 4294967296)
    return struct.pack("!II", seconds, fraction)


def serve_ntp(ntp_socket):
    while True:
        request, client = ntp_socket.recvfrom(512)
        if len(request) < 48:
            continue
        received = time.time()
        response = bytearray(48)
        response[0] = (request[0] & 0x38) | 4
        response[1] = 2
        response[2] = request[2]
        response[3] = 0xec
        response[8:12] = struct.pack("!I", 1 << 10)
        response[12:16] = b"LOCL"
        response[16:24] = ntp_timestamp(received - 1)
        response[24:32] = request[40:48]
        response[32:40] = ntp_timestamp(received)
        response[40:48] = ntp_timestamp(time.time())
        ntp_socket.sendto(response, client)


def make_listener():
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    return listener


def main():
    if len(sys.argv) not in (2, 4):
        raise SystemExit("usage: xymonnet-loopback.py READYFILE [CERT KEY]")

    ssh_listener = make_listener()
    bad_banner_listener = make_listener()
    ftp_listener = make_listener()
    telnet_listener = make_listener()

    tls_listener = None
    tls_context = None
    httpsd = None
    if len(sys.argv) == 4:
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(sys.argv[2], sys.argv[3])
        tls_listener = make_listener()
        httpsd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        httpsd.socket = tls_context.wrap_socket(
            httpsd.socket, server_side=True
        )

    dns_socket = None
    if os.environ.get("XYMONNET_DNS_FIXTURE") == "1":
        dns_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        dns_socket.bind(("127.0.0.1", 53))

    ntp_socket = None
    if os.environ.get("XYMONNET_NTP_FIXTURE") == "1":
        ntp_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        ntp_socket.bind(("127.0.0.1", 123))

    httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(sys.argv[1], "w", encoding="ascii") as ready:
        tls_port = tls_listener.getsockname()[1] if tls_listener else 0
        https_port = httpsd.server_port if httpsd else 0
        dns_ready = "1" if dns_socket else ""
        ntp_ready = "1" if ntp_socket else ""
        ready.write(
            f"{httpd.server_port} {ssh_listener.getsockname()[1]} "
            f"{bad_banner_listener.getsockname()[1]} "
            f"{ftp_listener.getsockname()[1]} "
            f"{telnet_listener.getsockname()[1]} {tls_port} {https_port} "
            f"{dns_ready or '0'} {ntp_ready or '0'}\n"
        )

    threading.Thread(
        target=serve_banner,
        args=(ssh_listener, b"SSH-2.0-xymon-fixture\r\n"),
        daemon=True,
    ).start()
    threading.Thread(
        target=serve_banner,
        args=(bad_banner_listener, b"unexpected banner\r\n"),
        daemon=True,
    ).start()
    threading.Thread(
        target=serve_banner,
        args=(ftp_listener, b"220 xymon FTP fixture\r\n"),
        daemon=True,
    ).start()
    threading.Thread(
        target=serve_telnet, args=(telnet_listener,), daemon=True
    ).start()
    if tls_listener and tls_context:
        threading.Thread(
            target=serve_ftps, args=(tls_listener, tls_context), daemon=True
        ).start()
    if httpsd:
        threading.Thread(target=httpsd.serve_forever, daemon=True).start()
    if dns_socket:
        threading.Thread(
            target=serve_dns, args=(dns_socket,), daemon=True
        ).start()
    if ntp_socket:
        threading.Thread(
            target=serve_ntp, args=(ntp_socket,), daemon=True
        ).start()
    httpd.serve_forever()


if __name__ == "__main__":
    main()
