#!/usr/bin/env python3
"""Real TUN, routing and DNS test. Must run in a separate user+network namespace.

unshare --user --map-root-user --net python3 tests/smoke_tun.py \
    --host-netns "$(readlink /proc/self/ns/net)" --service build/dde-mihomo-daemon
No system proxy changes, external nodes or downloads are used.
"""
import argparse
import contextlib
import http.server
import json
import os
import re
import shlex
import shutil
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request


def ip(*args):
    return subprocess.check_output(['ip', *args], text=True)


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_for(predicate, message, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.1)
    raise AssertionError(message)


class Target(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'dcc-proxy-tun-dns-http-ok'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host-netns', required=True)
    parser.add_argument('--service', default='/usr/lib/dcc-proxy/dde-mihomo-daemon')
    parser.add_argument('--core', default='/usr/lib/dcc-proxy/mihomo')
    args = parser.parse_args()
    assert os.geteuid() == 0, 'Run with unshare --user --map-root-user --net'
    assert os.readlink('/proc/self/ns/net') != args.host_netns, 'Refusing to change host network'
    assert {item['ifname'] for item in json.loads(ip('-j', 'link'))} == {'lo'}, 'Require fresh empty network namespace'
    service, core = str(Path(args.service).resolve()), str(Path(args.core).resolve())
    ip('link', 'set', 'lo', 'up')
    ip('link', 'add', 'uplink', 'type', 'dummy')
    ip('addr', 'add', '192.0.2.2/24', 'dev', 'uplink')
    ip('link', 'set', 'uplink', 'up')
    ip('route', 'add', 'default', 'via', '192.0.2.1', 'dev', 'uplink')
    ip('addr', 'add', '203.0.113.10/32', 'dev', 'lo')
    baseline_routes, baseline_rules = ip('route', 'show', 'table', 'all'), ip('rule')
    dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dns.bind(('127.0.0.1', 0))
    def serve_dns():
        while True:
            try:
                query, address = dns.recvfrom(4096)
                # One A question, answer fixture.test with the local HTTP target address.
                end = 12
                while query[end]:
                    end += query[end] + 1
                end += 5
                is_a = query[end-4:end-2] == b'\x00\x01'
                answer = b'\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04' + socket.inet_aton('203.0.113.10') if is_a else b''
                response = query[:2] + struct.pack('!HHHHH', 0x8180, 1, int(is_a), 0, 0) + query[12:end] + answer
                dns.sendto(response, address)
            except OSError:
                return
    threading.Thread(target=serve_dns, daemon=True).start()
    target = http.server.HTTPServer(('203.0.113.10', 0), Target)
    threading.Thread(target=target.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='dcc-proxy-tun-') as tmp:
        root = Path(tmp); (root / 'profiles').mkdir()
        profile = '00000000-0000-4000-8000-000000000002'
        controller, mixed, token = port(), port(), secrets.token_hex(24)
        deny_tun = root / 'deny-tun'
        setpriv = shutil.which('setpriv'); assert setpriv, 'Existing setpriv executable required'
        wrapper = root / 'test-core'
        wrapper.write_text('#!/bin/sh\nif test -f ' + shlex.quote(str(deny_tun)) + '; then exec ' + shlex.quote(setpriv) + ' --bounding-set=-net_admin ' + shlex.quote(core) + ' "$@"; fi\nexec ' + shlex.quote(core) + ' "$@"\n')
        wrapper.chmod(0o700)
        (root / 'profiles' / (profile + '.yaml')).write_text(
            'mixed-port: %d\nmode: rule\nlog-level: debug\nipv6: false\n'
            'dns:\n  enable: false\n  enhanced-mode: fake-ip\n  fake-ip-range: 198.18.0.1/16\n'
            '  fake-ip-filter: []\n  default-nameserver: [127.0.0.1:%d]\n  nameserver: [127.0.0.1:%d]\n'
            '  proxy-server-nameserver: [127.0.0.1:%d]\nrules: ["MATCH,DIRECT"]\n' %
            (mixed, dns.getsockname()[1], dns.getsockname()[1], dns.getsockname()[1]))
        (root / 'settings.json').write_text(json.dumps({'endpoint': 'http://127.0.0.1:%d' % controller,
            'secret': token, 'core': str(wrapper), 'active': profile, 'autoStart': False, 'autoSystemProxy': False, 'updateMinutes': 0}))
        (root / 'profiles.json').write_text(json.dumps({'profiles': [{'id': profile, 'name': 'namespace TUN fixture', 'url': ''}]}))
        env = dict(os.environ, DCC_PROXY_DATA_DIR=tmp)
        # Do not expose the desktop session/system buses to isolated tests.
        env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=/nonexistent'
        env['DBUS_SYSTEM_BUS_ADDRESS'] = 'unix:path=/nonexistent'
        daemon = subprocess.Popen([service, '--daemon'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        def command(action):
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(30); sock.connect(str(root / 'service.sock'))
                sock.sendall(json.dumps(action).encode() + b'\n'); data = b''
                while b'\n' not in data:
                    part = sock.recv(65536)
                    if not part: raise AssertionError('Daemon closed socket')
                    data += part
                return json.loads(data.split(b'\n')[0])
        def api(path):
            request = urllib.request.Request('http://127.0.0.1:%d%s' % (controller, path), headers={'Authorization': 'Bearer ' + token})
            with opener.open(request, timeout=3) as response: return json.load(response)
        def tun_options(options):
            result = command({'action': 'tun', 'options': options})
            assert not result.get('error'), result
            return result
        def tunnel_http():
            question = b''.join(bytes([len(label)]) + label.encode() for label in 'fixture.test'.split('.')) + b'\x00\x00\x01\x00\x01'
            query = b'\x12\x34' + struct.pack('!HHHHH', 0x0100, 1, 0, 0, 0) + question
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.settimeout(4); sock.sendto(query, ('8.8.8.8', 53)); response, _ = sock.recvfrom(4096)
            assert response[:2] == b'\x12\x34' and response[3] & 15 == 0, response.hex()
            fake = socket.inet_ntoa(response[-4:]); assert fake.startswith('198.18.'), fake
            with opener.open('http://%s:%d/' % (fake, target.server_port), timeout=6) as response:
                assert response.read() == b'dcc-proxy-tun-dns-http-ok'
        try:
            wait_for(lambda: (root / 'service.sock').exists(), 'Service did not start')
            result = command({'action': 'start'}); assert result.get('running') and not result.get('error'), result
            assert not command({'action': 'status'})['tun']['enable']
            initial_pid = result['pid']
            result = tun_options({'enable': True})
            assert result['pid'] == initial_pid, 'TUN enable restarted the core'
            assert result['tun']['enable'], result
            config = api('/configs'); assert config['tun']['enable'], config
            interface = json.loads(ip('-j', 'link', 'show', 'Mihomo'))[0]
            assert interface['mtu'] == 1500, interface
            tunnel_http()
            print('PASS: native Mixed TUN, auto-route, intercepted DNS and HTTP through fake IP')
            tun_options({'mtu': 1400, 'route-exclude-address': ['198.51.100.0/24']})
            assert command({'action': 'status'})['tun']['enable'], 'Settings save disabled TUN'
            assert command({'action': 'status'})['pid'] == initial_pid, 'Settings save restarted the core'
            assert json.loads(ip('-j', 'link', 'show', 'Mihomo'))[0]['mtu'] == 1400
            tunnel_http()
            old_pid = command({'action': 'status'})['pid']
            for invalid in ({'mtu': 0}, {'route-exclude-address': ['example.com']}, {'device': 'lo'}):
                result = command({'action': 'tun', 'options': invalid}); assert result.get('error'), result
                assert command({'action': 'status'})['pid'] == old_pid, 'Invalid settings restarted good core'
            print('PASS: saving preserves enable; MTU applied; invalid settings and device collision preserve running core')
            result = command({'action': 'repair-tun'}); assert not result.get('error'), result
            tunnel_http()
            print('PASS: resume repair reloads TUN, routes and DNS')
            tun_options({'enable': False})
            assert 'Mihomo' not in {item['ifname'] for item in json.loads(ip('-j', 'link'))}
            assert not api('/configs')['tun']['enable']
            assert re.search(r'dns:\n  enable: true', (root / 'work/config.yaml').read_text()), 'DNS disabled with TUN'
            assert ip('route', 'show', 'table', 'all') == baseline_routes, 'Routes not restored'
            assert ip('rule') == baseline_rules, 'Routing rules not restored'
            print('PASS: disabling removes interface and restores all routes/rules, DNS stays enabled')
            command({'action': 'stop'})
            deny_tun.touch()
            result = command({'action': 'start'}); assert result.get('running') and not result.get('error'), result
            denied_pid = result['pid']
            result = command({'action': 'tun', 'options': {'enable': True}})
            assert result.get('error'), 'Kernel TUN permission failure reported success'
            assert 'operation not permitted' in result['error'].lower(), result
            state = command({'action': 'status'})
            assert state['running'] and not state['tun']['enable'], state
            assert state['pid'] == denied_pid, 'TUN failure restarted the working core'
            assert not api('/configs')['tun']['enable']
            assert ip('route', 'show', 'table', 'all') == baseline_routes
            assert ip('rule') == baseline_rules
            print('PASS: real kernel permission failure rolls back TUN and restores a usable core')
            command({'action': 'stop'})
        except Exception:
            log = root / 'daemon.log'
            if log.exists(): print(log.read_text()[-16000:])
            raise
        finally:
            daemon.terminate()
            try: daemon.wait(timeout=5)
            except subprocess.TimeoutExpired: daemon.kill(); daemon.wait()
            target.shutdown(); dns.close()

if __name__ == '__main__':
    main()
