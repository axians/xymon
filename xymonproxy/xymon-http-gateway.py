#!/usr/bin/env python3

import argparse
import ipaddress
import logging
import socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


DEFAULT_ALLOWED_COMMANDS = frozenset(
    {"client", "data", "status", "usermsg"}
)


class MinicfgError(Exception):
    pass


class MinicfgRenderer:
    def __init__(self, hosts_file, config_dir, max_bytes):
        self.hosts_file = Path(hosts_file)
        self.config_dir = Path(config_dir)
        self.includes_dir = self.config_dir / "Includes"
        self.max_bytes = max_bytes

    def render(self, client_ip):
        machine = self._machine_for(client_ip)
        result = bytearray(
            b"[mrbig]\r\nmachine "
            + machine
            + b"\r\n"
        )
        client_file = self.config_dir / "Clients" / client_ip

        try:
            self._append_file(client_file, result, 1)
        except FileNotFoundError:
            self._append_file(self.includes_dir / "0.0.0.0", result, 1)

        return bytes(result)

    def _machine_for(self, client_ip):
        client_ip_bytes = client_ip.encode("ascii")
        try:
            hosts = self.hosts_file.read_bytes()
        except OSError as error:
            raise MinicfgError(
                f"cannot read hosts file {self.hosts_file}: {error}"
            ) from error

        for line in hosts.splitlines():
            fields = line.split()
            if len(fields) >= 2 and fields[0] == client_ip_bytes:
                return fields[1]

        return b"brokencfg-" + client_ip_bytes

    def _append_file(self, filename, result, level):
        if level > 5:
            raise MinicfgError("include nesting exceeds five levels")

        try:
            with filename.open("rb") as config_file:
                contents = config_file.read(self.max_bytes + 1)
        except FileNotFoundError:
            raise
        except OSError as error:
            raise MinicfgError(
                f"cannot read minicfg file {filename}: {error}"
            ) from error

        if len(contents) > self.max_bytes:
            raise MinicfgError(f"minicfg file {filename} is too large")

        for line in contents.splitlines(keepends=True):
            if line.startswith(b"!include "):
                include_name = line[9:].rstrip(b"\r\n")
                include_file = self._include_path(include_name)
                self._append_file(include_file, result, level + 1)
            else:
                self._append_bytes(result, line)

    def _include_path(self, include_name):
        try:
            relative_path = Path(include_name.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise MinicfgError("include name is not valid UTF-8") from error

        if relative_path.is_absolute() or ".." in relative_path.parts:
            raise MinicfgError("include path escapes the Includes directory")
        return self.includes_dir / relative_path

    def _append_bytes(self, result, data):
        if len(result) + len(data) > self.max_bytes:
            raise MinicfgError("rendered minicfg exceeds the size limit")
        result.extend(data)


class GatewayServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self,
        server_address,
        handler_class,
        backend_address,
        allowed_commands,
        max_body_bytes,
        backend_timeout,
        *,
        minicfg_hosts_file=None,
        minicfg_dir=None,
        minicfg_client_ip_header=None,
        max_minicfg_bytes=100000,
    ):
        super().__init__(server_address, handler_class)
        if bool(minicfg_hosts_file) != bool(minicfg_dir):
            raise ValueError(
                "minicfg_hosts_file and minicfg_dir must be "
                "configured together"
            )
        self.backend_address = backend_address
        self.allowed_commands = allowed_commands
        self.max_body_bytes = max_body_bytes
        self.backend_timeout = backend_timeout
        self.minicfg = (
            MinicfgRenderer(
                minicfg_hosts_file,
                minicfg_dir,
                max_minicfg_bytes,
            )
            if minicfg_hosts_file
            else None
        )
        self.minicfg_client_ip_header = minicfg_client_ip_header


class GatewayHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "XymonHTTPGateway/1.0"

    def do_POST(self):
        if self.path != "/":
            self.send_error(404, "Not Found")
            return

        if self.headers.get("Transfer-Encoding"):
            self.send_error(400, "Transfer-Encoding is not supported")
            return

        try:
            content_length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self.send_error(400, "Invalid Content-Length")
            return

        if content_length <= 0:
            self.send_error(400, "Request body is empty")
            return
        if content_length > self.server.max_body_bytes:
            self.send_error(413, "Request body is too large")
            return

        message = self.rfile.read(content_length)
        if len(message) != content_length:
            self.send_error(400, "Incomplete request body")
            return

        command = self._message_command(message)
        if command is None:
            self.send_error(400, "Invalid Xymon message")
            return
        if command not in self.server.allowed_commands:
            logging.warning(
                "Rejected Xymon command %r from %s",
                command,
                self.client_address[0],
            )
            self.send_error(403, "Xymon command is not allowed")
            return

        try:
            response = self._forward(message)
        except TimeoutError:
            logging.error("Timed out forwarding %s message", command)
            self.send_error(504, "Xymon proxy timed out")
            return
        except OSError as error:
            logging.error("Failed to forward %s message: %s", command, error)
            self.send_error(502, "Cannot reach Xymon proxy")
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def do_GET(self):
        if self.path != "/minicfg":
            self.send_error(405, "Method Not Allowed")
            return
        if self.server.minicfg is None:
            self.send_error(404, "Minicfg is not configured")
            return

        client_ip = self.client_address[0]
        if self.server.minicfg_client_ip_header:
            client_ip = self.headers.get(
                self.server.minicfg_client_ip_header, ""
            ).strip()
            if not client_ip:
                self.send_error(400, "Client IP header is missing")
                return

        try:
            client_ip = str(ipaddress.IPv4Address(client_ip))
        except ipaddress.AddressValueError:
            self.send_error(400, "Invalid client IPv4 address")
            return

        try:
            response = self.server.minicfg.render(client_ip)
        except (FileNotFoundError, MinicfgError) as error:
            logging.error("Cannot render minicfg for %s: %s", client_ip, error)
            self.send_error(500, "Cannot render minicfg")
            return

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def do_HEAD(self):
        self.send_error(405, "Method Not Allowed")

    def do_PUT(self):
        self.send_error(405, "Method Not Allowed")

    def do_DELETE(self):
        self.send_error(405, "Method Not Allowed")

    def log_message(self, format, *args):  # pylint: disable=redefined-builtin
        logging.info("%s - %s", self.client_address[0], format % args)

    @staticmethod
    def _message_command(message):
        message_parts = message.split(None, 1)
        first_token = message_parts[0] if message_parts else b""
        try:
            command = first_token.decode("ascii").lower()
        except UnicodeDecodeError:
            return None

        command = command.split("+", 1)[0]
        command = command.split("/", 1)[0]
        return command or None

    def _forward(self, message):
        chunks = []
        with socket.create_connection(
            self.server.backend_address,
            timeout=self.server.backend_timeout,
        ) as backend:
            backend.settimeout(self.server.backend_timeout)
            backend.sendall(message)
            backend.shutdown(socket.SHUT_WR)

            while True:
                chunk = backend.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)

        return b"".join(chunks)


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Forward HTTP POST bodies to a local Xymon proxy."
    )
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, default=1985)
    parser.add_argument("--backend-host", default="127.0.0.1")
    parser.add_argument("--backend-port", type=int, default=1984)
    parser.add_argument("--backend-timeout", type=float, default=10.0)
    parser.add_argument(
        "--max-body-bytes", type=int, default=(1024 * 1024) - 1
    )
    parser.add_argument(
        "--allow-command",
        action="append",
        default=[],
        metavar="COMMAND",
        help="allow an additional Xymon command; may be repeated",
    )
    parser.add_argument("--minicfg-hosts-file")
    parser.add_argument("--minicfg-dir")
    parser.add_argument("--minicfg-client-ip-header")
    parser.add_argument("--max-minicfg-bytes", type=int, default=100000)
    args = parser.parse_args()
    if bool(args.minicfg_hosts_file) != bool(args.minicfg_dir):
        parser.error(
            "--minicfg-hosts-file and --minicfg-dir must be used together"
        )
    return args


def main():
    args = parse_arguments()
    allowed_commands = DEFAULT_ALLOWED_COMMANDS | {
        command.lower() for command in args.allow_command
    }

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    server = GatewayServer(
        (args.listen_host, args.listen_port),
        GatewayHandler,
        (args.backend_host, args.backend_port),
        allowed_commands,
        args.max_body_bytes,
        args.backend_timeout,
        minicfg_hosts_file=args.minicfg_hosts_file,
        minicfg_dir=args.minicfg_dir,
        minicfg_client_ip_header=args.minicfg_client_ip_header,
        max_minicfg_bytes=args.max_minicfg_bytes,
    )
    logging.info(
        "Listening on %s:%d; forwarding to %s:%d; allowed commands: %s",
        args.listen_host,
        args.listen_port,
        args.backend_host,
        args.backend_port,
        ", ".join(sorted(allowed_commands)),
    )

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
