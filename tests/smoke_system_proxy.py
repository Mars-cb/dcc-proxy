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
    parser.add_argument('--core', default='/usr/lib/dcc-proxy/mihomo')
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
            assert not result.get('error'), result
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
            ok(action='proxy', mode='manual', port=16666)
            assert dbus('GetProxyMethod') == 'manual'
            assert [v.strip() for v in dbus('GetProxyIgnoreHosts').split(',')] == entries, repr(dbus('GetProxyIgnoreHosts'))
            entries = ['*.changed.test', '192.168.0.0/16', 'fc00::/7']
            ok(action='bypass', entries=entries)
            assert [v.strip() for v in dbus('GetProxyIgnoreHosts').split(',')] == entries, repr(dbus('GetProxyIgnoreHosts'))
            ok(action='proxy', mode='auto', port=16666)
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
            def free_port():
                with socket.socket() as sock:
                    sock.bind(('127.0.0.1', 0))
                    return sock.getsockname()[1]
            controller, mixed = free_port(), free_port()
            settings = json.loads((root/'settings.json').read_text())
            settings.update({'core': str(Path(args.core).resolve()),
                             'endpoint': 'http://127.0.0.1:%s'%controller,
                             'autoSystemProxy': True, 'updateMinutes': 0})
            (root/'settings.json').write_text(json.dumps(settings))
            fixture = root/'fixture.yaml'
            fixture.write_text('mixed-port: %s\nmode: direct\nrules: ["MATCH,DIRECT"]\n'%mixed)
            profile = subprocess.check_output([str(Path(args.service).resolve()), '--import', str(fixture), '--activate'], env=env, text=True).strip()
            started = ok(action='start', profile=profile)
            assert started['running'] and not started['busy']
            assert dbus('GetProxyMethod') == 'manual'
            assert dbus('GetProxy', 'https').splitlines() == ['127.0.0.1', str(mixed)]
            assert dbus('GetProxyIgnoreHosts') == 'localhost'
            assert '系统代理已启用' in started['message']
            started_pid = started['pid']
            bad = root/'invalid.yaml'
            bad.write_text('mixed-port: invalid-port\n')
            bad_profile = subprocess.check_output([str(Path(args.service).resolve()), '--import', str(bad)], env=env, text=True).strip()
            failed = command(action='start', profile=bad_profile)
            assert 'error' in failed
            assert command(action='status')['pid'] == started_pid
            assert dbus('GetProxy', 'https').splitlines() == ['127.0.0.1', str(mixed)]
            new_mixed = free_port()
            (root/'profiles'/('%s.yaml'%profile)).write_text('mixed-port: %s\nmode: direct\nrules: ["MATCH,DIRECT"]\n'%new_mixed)
            restarted = ok(action='restart', profile=profile)
            assert restarted['running']
            assert dbus('GetProxy', 'https').splitlines() == ['127.0.0.1', str(new_mixed)]
            changed = free_port()
            assert ok(action='ports', options={'mixed-port': changed})['running']
            assert dbus('GetProxy', 'https').splitlines() == ['127.0.0.1', str(changed)]
            ok(action='proxy', mode='auto', port=changed)
            changed_again = free_port()
            assert ok(action='ports', options={'mixed-port': changed_again})['running']
            assert dbus('GetProxyMethod') == 'auto'
            assert ('127.0.0.1:%d' % changed_again) in pac()
            with socket.socket() as occupied:
                occupied.bind(('127.0.0.1', 0)); occupied.listen(1)
                expected = state(); pid = command(action='status')['pid']
                failed = command(action='ports', options={'mixed-port': occupied.getsockname()[1]})
                assert failed.get('error') and state() == expected
                assert command(action='status')['pid'] == pid
            ok(action='proxy', mode='none', port=0)
            assert state() == before
            assert ok(action='ports', options={'mixed-port': free_port()})['running']
            assert state() == before, 'Changing listeners while proxy disabled must leave it disabled'
            print('PASS: port controls update owned manual/PAC proxies, reject conflicts and preserve explicit proxy-off')
            assert not ok(action='stop')['running']
            assert state() == before
            assert not (root/'system-proxy-before.json').exists()
            settings = json.loads((root/'settings.json').read_text())
            settings['autoSystemProxy'] = False
            (root/'settings.json').write_text(json.dumps(settings))
            assert ok(action='start', profile=profile)['running']
            assert state() == before, 'Explicit opt-out must preserve desktop proxy'
            ok(action='stop')
            assert state() == before
            print('PASS: start automatically enables proxy after readiness, failed validation preserves proxy, restart updates port, stop restores, explicit opt-out')
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
