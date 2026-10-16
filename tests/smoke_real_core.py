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
import signal
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
    headers_seen = {}
    subscription = b""
    subscription_user_agent = ""
    def do_GET(self):
        Target.headers_seen = dict(self.headers)
        if self.path == "/subscription":
            body = Target.subscription
            Target.subscription_user_agent = self.headers.get("User-Agent", "")
        else:
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
                    "active": "", "autoStart": False, "autoSystemProxy": False, "updateMinutes": 0}
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
                conn.request("GET", "http://127.0.0.1:%d/test" % target.server_port, headers={"User-Agent": "FixtureBrowser/1.0", "X-Fixture": "preserve"})
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
            result = command({"action": "start"})
            assert not result.get("error"), result
            assert result["running"], result
            assert result["profile"] == profile_id, result
            print("PASS first imported profile starts without a prior active selection")
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
            assert Target.headers_seen.get("User-Agent") == "FixtureBrowser/1.0" and Target.headers_seen.get("X-Fixture") == "preserve", Target.headers_seen
            assert not any(label in str(Target.headers_seen).lower() for label in ("clash", "verge", "mihomo", "party"))
            print("PASS DIRECT selection routes actual HTTP traffic and preserves application headers without injected software labels")
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
            with socket.socket() as occupied:
                occupied.bind(("127.0.0.1", 0))
                occupied.listen(1)
                blocked_port = occupied.getsockname()[1]
                profile.write_text(config.replace("mixed-port: %d"%mixed, "mixed-port: %d"%blocked_port))
                conflict = command({"action": "restart", "profile": profile_id})
                assert conflict.get("error") and "已被其他程序占用" in conflict["error"], conflict
                assert command({"action": "status"})["pid"] == original_pid
                assert through_proxy() == b"dcc-proxy-real-core-ok"
                print("PASS occupied port preserves running core and gives an actionable error")
            profile.write_text(config)
            api("/configs", "PATCH", {"mode": "direct"})
            assert api("/configs")["mode"] == "direct"
            print("PASS runtime mode change")
            new_mixed, http_port, socks_port = free_port(), free_port(), free_port()
            options = {"mixed-port": new_mixed, "port": http_port, "socks-port": socks_port,
                       "redir-port": 0, "tproxy-port": 0}
            changed = command({"action": "ports", "options": options})
            assert changed.get("running") and not changed.get("error"), changed
            mixed = new_mixed
            live = api("/configs")
            for key in ("mixed-port", "port", "socks-port"):
                assert live[key] == options[key], live
            assert through_proxy() == b"dcc-proxy-real-core-ok"
            proxy_http = http.client.HTTPConnection("127.0.0.1", http_port, timeout=3)
            proxy_http.request("GET", "http://127.0.0.1:%d/test" % target.server_port)
            assert proxy_http.getresponse().read() == b"dcc-proxy-real-core-ok"; proxy_http.close()
            with socket.create_connection(("127.0.0.1", socks_port), timeout=3) as socks:
                socks.sendall(b"\x05\x01\x00"); assert socks.recv(2) == b"\x05\x00"
                socks.sendall(b"\x05\x01\x00\x01\x7f\x00\x00\x01" + target.server_port.to_bytes(2, "big"))
                handshake = b""
                while len(handshake) < 10: handshake += socks.recv(10-len(handshake))
                assert handshake[:2] == b"\x05\x00", handshake
                socks.sendall(b"GET /test HTTP/1.0\r\nHost: localhost\r\n\r\n")
                response = b""
                while True:
                    data = socks.recv(4096)
                    if not data: break
                    response += data
                assert response.endswith(b"dcc-proxy-real-core-ok"), response
            print("PASS port controls change mixed/HTTP/SOCKS listeners and route actual traffic")
            original_pid = command({"action": "status"})["pid"]
            controlled = (root / "controlled.yaml").read_bytes()
            for transport in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                with socket.socket(socket.AF_INET, transport) as occupied:
                    occupied.bind(("127.0.0.1", 0))
                    if transport == socket.SOCK_STREAM: occupied.listen(1)
                    bad = dict(options); key = "port" if transport == socket.SOCK_STREAM else "mixed-port"
                    bad[key] = occupied.getsockname()[1]
                    conflict = command({"action": "ports", "options": bad})
                    assert conflict.get("error") and "已被其他程序占用" in conflict["error"], conflict
                    assert command({"action": "status"})["pid"] == original_pid
                    assert (root / "controlled.yaml").read_bytes() == controlled
                    assert through_proxy() == b"dcc-proxy-real-core-ok"
            for bad in ({"mixed-port": controller}, {"mixed-port": http_port, "port": http_port},
                        {"mixed-port": 0}, {"port": 65536}, {"port": 1.5}):
                invalid = command({"action": "ports", "options": bad})
                assert invalid.get("error"), invalid
                assert command({"action": "status"})["pid"] == original_pid
                assert (root / "controlled.yaml").read_bytes() == controlled
            print("PASS TCP/UDP conflicts, duplicate/controller/out-of-range ports preserve the running core and saved configuration")
            assert not command({"action": "stop"})["running"]
            offline = command({"action": "ports", "options": {"mixed-port": new_mixed, "port": 0, "socks-port": 0}})
            assert not offline.get("error") and not offline["running"], offline
            assert command({"action": "start"})["running"]
            assert api("/configs")["port"] == 0 and api("/configs")["socks-port"] == 0
            assert through_proxy() == b"dcc-proxy-real-core-ok"
            print("PASS offline port save, optional listener disable and persistence across restart")
            records = json.loads((root / "profiles.json").read_text())
            records["profiles"][0].update({"url": "http://127.0.0.1:%d/subscription" % target.server_port,
                                            "updated": "1970-01-01T00:00:00Z"})
            (root / "profiles.json").write_text(json.dumps(records))
            Target.subscription = config.encode()
            before_update_pid = command({"action": "status"})["pid"]
            assert command({"action": "update"}).get("ok")
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                metadata = json.loads((root / "profiles.json").read_text())["profiles"][0]
                state = command({"action": "status"})
                if metadata.get("updated") != "1970-01-01T00:00:00Z" and state.get("running") and state.get("pid") != before_update_pid and not state.get("busy"):
                    break
                time.sleep(.1)
            else: raise AssertionError("Active subscription update did not finish")
            assert Target.subscription_user_agent == "Mozilla/5.0", Target.subscription_user_agent
            assert through_proxy() == b"dcc-proxy-real-core-ok"
            print("PASS subscription refresh uses a neutral User-Agent, preserves controlled ports and applies the active configuration")
            crashed_pid = command({"action": "status"})["pid"]
            os.kill(crashed_pid, signal.SIGKILL)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                state = command({"action": "status"})
                if state.get("running") and state.get("pid") != crashed_pid and not state.get("busy"):
                    assert through_proxy() == b"dcc-proxy-real-core-ok"; break
                time.sleep(.1)
            else: raise AssertionError("Unexpected kernel crash did not recover")
            print("PASS unexpected kernel exit restarts the managed core and restores actual proxy traffic")
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
