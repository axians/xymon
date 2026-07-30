import http.client
import importlib.util
import socket
import tempfile
import threading
import unittest
from pathlib import Path


GATEWAY_PATH = (
    Path(__file__).resolve().parents[2]
    / "xymonproxy"
    / "xymon-http-gateway.py"
)
SPEC = importlib.util.spec_from_file_location(
    "xymon_http_gateway", GATEWAY_PATH
)
GATEWAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATEWAY)


class FakeXymonProxy:
    def __init__(self, response=b""):
        self.response = response
        self.message = b""
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.address = self.listener.getsockname()
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def start(self):
        self.thread.start()

    def _serve(self):
        try:
            connection, _ = self.listener.accept()
        except OSError:
            return
        with connection:
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    break
                self.message += chunk
            connection.sendall(self.response)
        self.listener.close()

    def stop(self):
        if self.thread.is_alive():
            with socket.create_connection(
                self.address, timeout=1
            ) as connection:
                connection.shutdown(socket.SHUT_WR)
                while connection.recv(65536):
                    pass
        self.listener.close()
        self.thread.join(timeout=2)


class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.minicfg_dir = Path(self.temp_dir.name) / "minicfg"
        (self.minicfg_dir / "Clients").mkdir(parents=True)
        (self.minicfg_dir / "Includes").mkdir()
        self.hosts_file = Path(self.temp_dir.name) / "hosts.cfg"
        self.hosts_file.write_text(
            "192.0.2.10 mrbig.example.com # mrbig\n",
            encoding="ascii",
        )
        self.backend = FakeXymonProxy(b"remote client configuration\n")
        self.backend.start()
        self.server = GATEWAY.GatewayServer(
            ("127.0.0.1", 0),
            GATEWAY.GatewayHandler,
            self.backend.address,
            GATEWAY.DEFAULT_ALLOWED_COMMANDS,
            128,
            2,
            minicfg_hosts_file=self.hosts_file,
            minicfg_dir=self.minicfg_dir,
            minicfg_client_ip_header="X-Real-IP",
            max_minicfg_bytes=100000,
        )
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.backend.stop()
        self.temp_dir.cleanup()

    def request(self, method, body=None, path="/", headers=None):
        connection = http.client.HTTPConnection(
            *self.server.server_address, timeout=2
        )
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        result = response.status, response.read()
        connection.close()
        return result

    def test_forwards_raw_client_message_and_returns_response(self):
        message = (
            b"client host.example.com.powershell powershell XymonPS\n"
            b"[data]\n"
        )

        status, response = self.request("POST", message)

        self.assertEqual(status, 200)
        self.assertEqual(response, self.backend.response)
        self.assertEqual(self.backend.message, message)

    def test_rejects_forbidden_command(self):
        status, _ = self.request("POST", b"drop host.example.com\n")

        self.assertEqual(status, 403)
        self.assertEqual(self.backend.message, b"")

    def test_rejects_combo_command_dispatch(self):
        status, _ = self.request(
            "POST", b"combo\n\ndrop host.example.com\n"
        )

        self.assertEqual(status, 403)
        self.assertEqual(self.backend.message, b"")

    def test_accepts_status_lifetime_and_group_suffixes(self):
        message = b"status+10m/group:windows host.example.com.cpu green OK\n"

        status, _ = self.request("POST", message)

        self.assertEqual(status, 200)
        self.assertEqual(self.backend.message, message)

    def test_rejects_get(self):
        status, _ = self.request("GET")

        self.assertEqual(status, 405)
        self.assertEqual(self.backend.message, b"")

    def test_rejects_oversized_body(self):
        status, _ = self.request("POST", b"status " + (b"x" * 128))

        self.assertEqual(status, 413)
        self.assertEqual(self.backend.message, b"")

    def test_returns_minicfg_selected_by_client_ip(self):
        (self.minicfg_dir / "Clients" / "192.0.2.10").write_bytes(
            b"setting one\r\n!include common\r\nsetting two\r\n"
        )
        (self.minicfg_dir / "Includes" / "common").write_bytes(
            b"included yes\n"
        )

        status, response = self.request(
            "GET",
            path="/minicfg",
            headers={"X-Real-IP": "192.0.2.10"},
        )

        self.assertEqual(status, 200)
        self.assertEqual(
            response,
            b"[mrbig]\r\nmachine mrbig.example.com\r\n"
            b"setting one\r\nincluded yes\nsetting two\r\n",
        )
        self.assertEqual(self.backend.message, b"")

    def test_minicfg_falls_back_to_default_client_config(self):
        (self.minicfg_dir / "Includes" / "0.0.0.0").write_bytes(
            b"default setting\r\n"
        )

        status, response = self.request(
            "GET",
            path="/minicfg",
            headers={"X-Real-IP": "192.0.2.20"},
        )

        self.assertEqual(status, 200)
        self.assertEqual(
            response,
            b"[mrbig]\r\nmachine brokencfg-192.0.2.20\r\n"
            b"default setting\r\n",
        )

    def test_minicfg_requires_client_ip_header(self):
        status, _ = self.request("GET", path="/minicfg")

        self.assertEqual(status, 400)

    def test_minicfg_rejects_invalid_client_ip_header(self):
        status, _ = self.request(
            "GET",
            path="/minicfg",
            headers={"X-Real-IP": "192.0.2.10, 198.51.100.20"},
        )

        self.assertEqual(status, 400)

    def test_minicfg_rejects_include_path_traversal(self):
        (self.minicfg_dir / "Clients" / "192.0.2.10").write_bytes(
            b"!include ../Clients/192.0.2.10\r\n"
        )

        status, _ = self.request(
            "GET",
            path="/minicfg",
            headers={"X-Real-IP": "192.0.2.10"},
        )

        self.assertEqual(status, 500)


if __name__ == "__main__":
    unittest.main()
