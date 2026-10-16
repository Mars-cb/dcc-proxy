#!/usr/bin/env python3
"""Exercise the installed DDE proxy interface and PAC; restore desktop settings."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--service', required=True)
    parser.add_argument('--allow-system-proxy', action='store_true',
                        help='temporarily change the current desktop proxy and restore it')
    args = parser.parse_args()
    if not args.allow_system_proxy:
        parser.error('This integration test requires --allow-system-proxy')
    service = 'org.deepin.dde.Network1'
    object_path = '/org/deepin/dde/Network1'
    def dbus(method, *params):
        return subprocess.check_output(['qdbus', service, object_path, service+'.'+method, *params], text=True, stderr=subprocess.DEVNULL).strip()
    try:
        dbus('GetProxyMethod')
    except subprocess.CalledProcessError:
        service = 'com.deepin.daemon.Network'
        object_path = '/com/deepin/daemon/Network'
    def state():
        result = {key: dbus(key) for key in ['GetProxyMethod', 'GetProxyIgnoreHosts', 'GetAutoProxy']}
        result.update({kind: dbus('GetProxy', kind) for kind in ['http', 'https', 'socks', 'ftp']})
        return result
    before = state()
    with tempfile.TemporaryDirectory(prefix='dcc-proxy-system-test-') as tmp:
        root = Path(tmp)
        env = dict(os.environ, DCC_PROXY_DATA_DIR=tmp)
        process = subprocess.Popen([str(Path(args.service).resolve()), '--daemon'], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        def command(**data):
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(15)
                sock.connect(str(root/'service.sock'))
                sock.sendall(json.dumps(data).encode()+b'\n')
                result = b''
                while b'\n' not in result:
                    block = sock.recv(65536)
                    if not block:
                        raise RuntimeError('Background disconnected')
                    result += block
                return json.loads(result.split(b'\n')[0])
        def ok(**data):
            result = command(**data)
            assert 'error' not in result, result
            return result
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        def pac():
            with opener.open('http://127.0.0.1:19091/proxy.pac', timeout=3) as reply:
                assert reply.headers['Content-Type'] == 'application/x-ns-proxy-autoconfig'
                return reply.read().decode()
        try:
            for _ in range(100):
                if (root/'service.sock').exists():
                    break
                if process.poll() is not None:
                    raise RuntimeError(process.stderr.read().decode())
                time.sleep(.05)
            entries = ['localhost', '*.example.com', '10.0.0.0/8', '::1']
            ok(action='bypass', entries=entries)
            assert state() == before, 'Saving while disabled changed desktop proxy'
            ok(action='proxy', mode='manual', port=17890)
            assert dbus('GetProxyMethod') == 'manual'
            assert [v.strip() for v in dbus('GetProxyIgnoreHosts').split(',')] == entries, repr(dbus('GetProxyIgnoreHosts'))
            entries = ['*.changed.test', '192.168.0.0/16', 'fc00::/7']
            ok(action='bypass', entries=entries)
            assert [v.strip() for v in dbus('GetProxyIgnoreHosts').split(',')] == entries, repr(dbus('GetProxyIgnoreHosts'))
            ok(action='proxy', mode='auto', port=17890)
            assert dbus('GetProxyMethod') == 'auto'
            assert dbus('GetAutoProxy') == 'http://127.0.0.1:19091/proxy.pac'
            assert '*.changed.test' in pac()
            ok(action='bypass', entries=['*.updated.test'])
            assert '*.updated.test' in pac() and '*.changed.test' not in pac()
            rejected = command(action='bypass', entries=['https://invalid.test'])
            assert 'error' in rejected
            assert dbus('GetProxyIgnoreHosts') == '*.updated.test'
            assert json.loads((root/'settings.json').read_text())['proxyBypass'] == ['*.updated.test']
            ok(action='bypass', entries=[])
            assert dbus('GetProxyIgnoreHosts') == ''
            assert 'var bypassDomains=[];' in pac() and 'var bypassNetworks=[];' in pac()
            settings = json.loads((root/'settings.json').read_text())
            settings['proxyBypass'] = ['https://invalid-backup.test']
            (root/'settings.json').write_text(json.dumps(settings))
            ok(action='proxy', mode='none', port=0)
            assert state() == before, 'Original proxy settings were not restored'
            assert not (root/'system-proxy-before.json').exists()
            ok(action='bypass', entries=['localhost'])
            assert state() == before
            print('PASS: save while off, native manual bypass, live edits, dynamic PAC, empty list, invalid input, full restore')
        finally:
            try:
                ok(action='proxy', mode='none', port=0)
                ok(action='quit')
            finally:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)
            assert state() == before, 'Cleanup did not restore desktop settings'


if __name__ == '__main__':
    main()
