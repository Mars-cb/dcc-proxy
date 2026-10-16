#!/usr/bin/env python3
"""Real-core integration test; uses only Python stdlib and an isolated data dir."""
import argparse
import contextlib
import http.client
import http.server
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Target(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"dcc-proxy-real-core-ok"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", default="/usr/lib/dcc-proxy/dde-mihomo-daemon")
    parser.add_argument("--core", default="/usr/lib/dcc-proxy/mihomo")
    parser.add_argument("--preview")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dcc-proxy-smoke-") as tmp:
        root = Path(tmp)
        (root / "profiles").mkdir()
        controller, mixed = free_port(), free_port()
        token = secrets.token_hex(24)
        profile_id = "00000000-0000-4000-8000-000000000001"
        group = "选择 /?#日本"
        config = ("mixed-port: %d\nmode: rule\nlog-level: info\n"
                  "proxy-groups:\n  - name: '%s'\n    type: select\n"
                  "    proxies: [DIRECT, REJECT]\nrules:\n  - MATCH,%s\n") % (mixed, group, group)
        profile = root / "profiles" / (profile_id + ".yaml")
        profile.write_text(config)
        settings = {"endpoint": "http://127.0.0.1:%d" % controller,
                    "secret": token, "core": str(Path(args.core).resolve()),
                    "active": profile_id, "autoStart": False, "autoSystemProxy": False, "updateMinutes": 0}
        (root / "settings.json").write_text(json.dumps(settings))
        (root / "profiles.json").write_text(json.dumps({"profiles": [
            {"id": profile_id, "name": "integration fixture", "url": ""}]}))
        env = dict(os.environ, DCC_PROXY_DATA_DIR=tmp, QT_QPA_PLATFORM="offscreen")
        daemon = subprocess.Popen([args.service, "--daemon"], env=env, start_new_session=True,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        target = http.server.HTTPServer(("127.0.0.1", 0), Target)
        threading.Thread(target=target.serve_forever, daemon=True).start()
        preview = None

        def command(action):
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(20)
                s.connect(str(root / "service.sock"))
                s.sendall(json.dumps(action).encode() + b"\n")
                data = b""
                while b"\n" not in data:
                    part = s.recv(65536)
                    if not part:
                        raise RuntimeError("Service closed without response")
                    data += part
                return json.loads(data.split(b"\n")[0])

        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def api(path, method="GET", data=None):
            request = urllib.request.Request("http://127.0.0.1:%d%s" % (controller, path),
                                             data=None if data is None else json.dumps(data).encode(),
                                             headers={"Authorization": "Bearer " + token,
                                                      "Content-Type": "application/json"}, method=method)
            with opener.open(request, timeout=3) as response:
                raw = response.read()
                return json.loads(raw) if raw else {}

        def through_proxy():
            conn = http.client.HTTPConnection("127.0.0.1", mixed, timeout=3)
            try:
                conn.request("GET", "http://127.0.0.1:%d/test" % target.server_port)
                return conn.getresponse().read()
            finally:
                conn.close()

        try:
            for _ in range(100):
                if (root / "service.sock").exists():
                    break
                if daemon.poll() is not None:
                    raise RuntimeError("Daemon exited before listening")
                time.sleep(0.1)
            result = command({"action": "start", "profile": profile_id})
            assert not result.get("error"), result
            assert result["running"], result
            for _ in range(100):
                try:
                    version = api("/version")
                    break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.1)
            else:
                raise RuntimeError("Core API did not become ready: " + (root / "daemon.log").read_text())
            print("PASS official core:", version["version"])
            path = "/proxies/" + urllib.parse.quote(group, safe="")
            api(path, "PUT", {"name": "DIRECT"})
            assert api(path)["now"] == "DIRECT"
            assert through_proxy() == b"dcc-proxy-real-core-ok"
            print("PASS DIRECT selection routes actual HTTP traffic")
            api(path, "PUT", {"name": "REJECT"})
            assert api(path)["now"] == "REJECT"
            rejected = False
            try:
                rejected = through_proxy() != b"dcc-proxy-real-core-ok"
            except (OSError, http.client.HTTPException):
                rejected = True
            assert rejected
            print("PASS REJECT selection blocks actual HTTP traffic")
            api(path, "PUT", {"name": "DIRECT"})
            original_pid = command({"action": "status"})["pid"]
            if args.preview:
                preview = subprocess.Popen([args.preview, "Proxies"], env=env,
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(2)
                assert preview.poll() is None, "Preview failed to open"
                preview.terminate()
                preview.wait(timeout=5)
                assert command({"action": "status"})["pid"] == original_pid
                assert through_proxy() == b"dcc-proxy-real-core-ok"
                print("PASS closing plugin window leaves the same core running")
            profile.write_text("mixed-port: invalid-port\n")
            invalid = command({"action": "restart", "profile": profile_id})
            assert invalid.get("error"), invalid
            assert command({"action": "status"})["pid"] == original_pid
            assert through_proxy() == b"dcc-proxy-real-core-ok"
            print("PASS invalid configuration preserves running core")
            profile.write_text(config)
            api("/configs", "PATCH", {"mode": "direct"})
            assert api("/configs")["mode"] == "direct"
            print("PASS runtime mode change")
            assert not command({"action": "stop"})["running"]
            print("PASS stop releases managed core")
        finally:
            if preview is not None and preview.poll() is None:
                preview.terminate()
                preview.wait(timeout=5)
            with contextlib.suppress(Exception):
                command({"action": "quit"})
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.terminate()
                daemon.wait(timeout=5)
            target.shutdown()
    print("All real-core integration checks passed.")


if __name__ == "__main__":
    main()
