#!/usr/bin/env python3
"""Failure injection in a temporary store, with a private fake gsettings executable.

No desktop bus or real system proxy settings are accessible to the test daemon.
Only child processes created by this fixture are signalled.
"""
import argparse
import contextlib
import http.client
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_for(predicate, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError('Fixture condition timed out')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--service', required=True)
    parser.add_argument('--core', required=True)
    parser.add_argument('--restart-binary', help='Upgrade the fixture daemon on disk before testing full service restart')
    parser.add_argument('--systemd-fixture', action='store_true', help='Use an isolated fake systemctl; never connects to the host manager')
    args = parser.parse_args()
    service, core = str(Path(args.service).resolve()), str(Path(args.core).resolve())
    with tempfile.TemporaryDirectory(prefix='dcc-proxy-stability-') as tmp:
        root = Path(tmp) / 'dcc-proxy'
        root.mkdir()
        (root / 'profiles').mkdir()
        (root / 'bin').mkdir()
        fixture_service = root / 'bin/dde-mihomo-daemon'
        shutil.copy2(service, fixture_service)
        service = str(fixture_service)
        settings_state = root / 'fake-proxy.json'
        fake = root / 'bin/gsettings'
        fake.write_text('#!' + sys.executable + '\n' + r'''
import json,os,sys
from pathlib import Path
p=Path(os.environ['DCC_PROXY_FAKE_GSETTINGS'])
args=sys.argv[1:]
state=json.loads(p.read_text()) if p.exists() else {}
if args==['list-schemas']:
    print('org.gnome.system.proxy')
elif args[0]=='get':
    key=args[1]+'/'+args[2]
    default={'mode':"'none'",'host':"''",'port':'0','autoconfig-url':"''",'ignore-hosts':"['localhost']"}.get(args[2],'false')
    print(state.get(key,default))
elif args[0]=='set':
    key=args[1]+'/'+args[2]
    value=args[3]
    if args[2] in ('host','mode','autoconfig-url'):
        value="'"+value.strip("'")+"'"
    state[key]=value
    temp=p.with_suffix('.part'); temp.write_text(json.dumps(state)); temp.replace(p)
else:
    raise SystemExit(1)
''')
        fake.chmod(0o700)
        good = '00000000-0000-4000-8000-000000000021'
        bad = '00000000-0000-4000-8000-000000000022'
        mixed, controller = free_port(), free_port()
        original = ('mixed-port: %d\nmode: direct\nrules: ["MATCH,DIRECT"]\n' % mixed).encode()

        class Target(http.server.BaseHTTPRequestHandler):
            subscription = original

            def do_GET(self):
                body = self.subscription if self.path == '/subscription' else b'fixture-proxy-traffic-ok'
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *_):
                pass

        server = http.server.HTTPServer(('127.0.0.1', 0), Target)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        wrapper = root / 'core-wrapper'
        wrapper.write_text('#!' + sys.executable + '\nimport os,sys\nfrom pathlib import Path\n'
                           'args=sys.argv[1:]\n'
                           'if "-t" not in args and "audit-fail-start: true" in Path(args[args.index("-f")+1]).read_text(): sys.exit(42)\n'
                           'os.execv(' + repr(core) + ', [' + repr(core) + ', *args])\n')
        wrapper.chmod(0o700)
        profile = root / 'profiles' / (good + '.yaml')
        profile.write_bytes(original)
        (root / 'profiles' / (bad + '.yaml')).write_bytes(original + b'audit-fail-start: true\n')
        (root / 'profiles.json').write_text(json.dumps({'profiles': [
            {'id': good, 'name': 'good fixture', 'url': 'http://127.0.0.1:%d/subscription' % server.server_port},
            {'id': bad, 'name': 'runtime failure fixture', 'url': ''}]}))
        (root / 'settings.json').write_text(json.dumps({'core': str(wrapper), 'endpoint': 'http://127.0.0.1:%d' % controller,
            'secret': 'fixture-token', 'active': good, 'autoStart': False, 'autoSystemProxy': True, 'updateMinutes': 0}))
        env = dict(os.environ, DCC_PROXY_DATA_DIR=str(root), DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent',
                   DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent', DCC_PROXY_FAKE_GSETTINGS=str(settings_state),
                   PATH=str(root / 'bin') + os.pathsep + os.environ.get('PATH', ''))
        if args.systemd_fixture:
            env.pop('DCC_PROXY_DATA_DIR', None)
            env['XDG_CONFIG_HOME'] = tmp
            controller_script = root / 'bin/systemctl'
            controller_script.write_text('#!' + sys.executable + '\n' +
                'import os,sys,signal,subprocess,time\nfrom pathlib import Path\n' +
                'root=Path(' + repr(str(root)) + ')\n' +
                'assert os.environ["XDG_CONFIG_HOME"] == str(root.parent)\n' +
                'args=sys.argv[1:]\nlock=root/"daemon.lock"\n' +
                'if args==["--user","show","--property=MainPID","--value","dde-mihomo-daemon.service"]:\n print(lock.read_text().splitlines()[0] if lock.exists() else "0")\n' +
                'elif args==["--user","restart","dde-mihomo-daemon.service"]:\n' +
                ' pid=int(lock.read_text().splitlines()[0])\n' +
                ' assert b"--daemon" in Path("/proc/%d/cmdline"%pid).read_bytes().split(b"\\0")\n' +
                ' assert ("XDG_CONFIG_HOME="+str(root.parent)).encode() in Path("/proc/%d/environ"%pid).read_bytes().split(b"\\0")\n' +
                ' os.kill(pid,signal.SIGTERM)\n deadline=time.monotonic()+10\n' +
                ' while lock.exists() and time.monotonic()<deadline: time.sleep(.1)\n' +
                ' assert not lock.exists()\n' +
                ' subprocess.Popen([str(root/"bin/dde-mihomo-daemon"),"--daemon"],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)\n' +
                'else: sys.exit(1)\n')
            controller_script.chmod(0o700)
        assert Path(shutil.which('gsettings', path=env['PATH'])).resolve() == fake.resolve()
        assert env['DBUS_SESSION_BUS_ADDRESS'] == env['DBUS_SYSTEM_BUS_ADDRESS'] == 'unix:path=/nonexistent'
        daemon = None
        fixture_pids = set()

        def command(action, **values):
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(35)
                sock.connect(str(root / 'service.sock'))
                sock.sendall(json.dumps(dict(action=action, **values)).encode() + b'\n')
                data = b''
                while b'\n' not in data:
                    chunk = sock.recv(65536)
                    assert chunk, 'Fixture command disconnected'
                    data += chunk
                result = json.loads(data.split(b'\n')[0])
                if result.get("running") and result.get("pid"): fixture_pids.add(int(result["pid"]))
                return result

        def launch():
            nonlocal daemon
            daemon = subprocess.Popen([service, '--daemon'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            wait_for(lambda: (root / 'service.sock').exists())
            wait_for(lambda: command('status'))

        def ok(action, **values):
            result = command(action, **values)
            assert not result.get('error'), result
            return result

        def state():
            return command('status')

        def mode():
            return json.loads(settings_state.read_text()).get('org.gnome.system.proxy/mode', "'none'")

        def traffic():
            client = http.client.HTTPConnection('127.0.0.1', mixed, timeout=3)
            try:
                client.request('GET', 'http://127.0.0.1:%d/target' % server.server_port)
                return client.getresponse().read() == b'fixture-proxy-traffic-ok'
            finally:
                client.close()

        def refreshed(previous_time):
            records = json.loads((root / 'profiles.json').read_text())['profiles']
            return next(p for p in records if p['id'] == good).get('updated') != previous_time and not state().get('busy')

        try:
            launch()
            first = ok('start')
            assert first['running'] and traffic()
            assert mode() == "'manual'"
            ok('proxy', mode='auto', port=mixed, pac='http://127.0.0.1:9/fixture.pac')
            pid = state()['pid']
            old_time = next(p for p in json.loads((root / 'profiles.json').read_text())['profiles'] if p['id'] == good).get('updated')
            ok('update')
            wait_for(lambda: refreshed(old_time))
            assert state()['pid'] == pid and mode() == "'auto'" and traffic()
            print('PASS identical subscription update keeps PID, traffic and PAC mode')

            Target.subscription = original + b'log-level: warning\n'
            time.sleep(1.1)  # Profile timestamps have second precision.
            old_time = next(p for p in json.loads((root / 'profiles.json').read_text())['profiles'] if p['id'] == good).get('updated')
            ok('update')
            wait_for(lambda: refreshed(old_time))
            wait_for(lambda: (root / 'work/config.yaml').read_bytes().find(b'log-level: warning') >= 0 and not state().get('busy'))
            assert state()['pid'] == pid and mode() == "'auto'" and traffic()
            print('PASS changed valid subscription hot reloads without replacing the core or proxy mode')

            before = profile.read_bytes()
            Target.subscription = b'mixed-port: invalid-port\n'
            ok('update')
            wait_for(lambda: 'candidate rejected' in (root / 'daemon.log').read_text())
            assert profile.read_bytes() == before and state()['pid'] == pid and traffic()
            print('PASS invalid candidate preserves disk subscription and the usable core')

            if args.restart_binary:
                replacement = root / 'bin/new-daemon'
                shutil.copy2(str(Path(args.restart_binary).resolve()), replacement)
                replacement.replace(fixture_service)

            def restart_service(expect_error=False):
                token = str(uuid.uuid4())
                result = subprocess.run([service, '--restart-service', token], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
                response = json.loads(result.stdout)
                assert bool(response.get('error')) == expect_error, response
                assert result.returncode == (1 if expect_error else 0), response
                return response

            profile.write_text('mixed-port: invalid-port\n')
            restart_service(expect_error=True)
            assert state()['pid'] == pid and state()['running'] and traffic()
            profile.write_bytes(before)
            previous_daemon = state().get('daemonPid', daemon.pid)
            resumed = restart_service()
            assert resumed['pid'] != pid and resumed['daemonPid'] != previous_daemon and resumed['version']
            assert mode() == "'auto'" and traffic()
            daemon.wait(timeout=5)
            print('PASS full restart loads the replacement daemon and core; preflight failure leaves both running; PAC preserved')

            for proxy_mode, expected in [('none', "'none'"), ('manual', "'manual'")]:
                ok('proxy', mode=proxy_mode, port=mixed)
                previous = state()
                resumed = restart_service()
                assert resumed['pid'] != previous['pid'] and resumed['daemonPid'] != previous['daemonPid']
                assert mode() == expected and traffic()
            print('PASS full restart preserves explicit proxy-off and manual modes despite automatic proxy being enabled')

            ok('proxy', mode='auto', port=mixed, pac='http://127.0.0.1:9/fixture.pac')
            profile.write_bytes(before + b'audit-fail-start: true\n')
            restored = restart_service(expect_error=True)
            assert restored['running'] and '恢复重启前' in restored['error'] and mode() == "'auto'" and traffic()
            profile.write_bytes(before)
            print('PASS full restart runtime failure falls back to the previously working runtime')

            failed = command('start', profile=bad)
            assert failed.get('error') and failed.get('rolledBack') and failed.get('running'), failed
            assert state()['profile'] == good and mode() == "'auto'" and traffic()
            print('PASS runtime startup failure restores last working config and PAC mode')

            profile.write_text('mixed-port: invalid-port\n')  # Simulate an out-of-band bad file.
            for attempt in range(4):
                crashed = state()['pid']
                assert crashed > 0
                os.kill(crashed, signal.SIGKILL)
                if attempt < 3:
                    wait_for(lambda: state().get('running') and state().get('pid') != crashed and not state().get('busy'))
                    assert traffic()
                else:
                    wait_for(lambda: not state().get('running') and '暂停' in state().get('error', ''))
            assert mode() == "'none'"
            print('PASS recovery uses last-good config and restores proxy after bounded repeated failures')

            profile.write_bytes(before)
            ok('start')
            external = json.loads(settings_state.read_text())
            external['org.gnome.system.proxy/mode'] = "'auto'"
            external['org.gnome.system.proxy/autoconfig-url'] = "'https://external.example/pac'"
            settings_state.write_text(json.dumps(external))
            ok('stop')
            assert json.loads(settings_state.read_text()) == external
            print('PASS stop preserves proxy settings changed externally')

            # A different fixture core instance, not the desktop core, is left orphaned.
            settings = json.loads((root / 'settings.json').read_text()); settings['core'] = core
            (root / 'settings.json').write_text(json.dumps(settings))
            owned = ok('start')['pid']
            assert (root / 'work/core-instance.json').exists()
            parent = state()['daemonPid']
            os.kill(parent, signal.SIGKILL)
            if parent == daemon.pid: daemon.wait(timeout=5)
            assert Path('/proc/%d' % owned).exists()
            with contextlib.suppress(FileNotFoundError): (root / 'service.sock').unlink()
            launch()
            orphan = state()['orphan']
            assert orphan.get('verified') and orphan['pid'] == owned, orphan
            assert command('start').get('error') and traffic()
            saved_profile = profile.read_bytes()
            profile.write_text('mixed-port: invalid-port\n')
            assert command('recover-orphan').get('error') and traffic()
            assert Path('/proc/%d' % owned).exists()
            profile.write_bytes(saved_profile)
            recovered = ok('recover-orphan')
            assert recovered['running'] and recovered['pid'] != owned and traffic()
            print('PASS orphan detection refuses automatic takeover and explicit recovery affects only the verified fixture')
            ok('stop')
        except Exception:
            log = root / 'daemon.log'
            if log.exists():
                print(log.read_text()[-10000:])  # All fixture data is synthetic.
            raise
        finally:
            with contextlib.suppress(Exception): command('quit')
            if daemon and daemon.poll() is None:
                try: daemon.wait(timeout=8)
                except subprocess.TimeoutExpired: daemon.terminate(); daemon.wait(timeout=5)
            for pid in fixture_pids:
                with contextlib.suppress(FileNotFoundError, ProcessLookupError):
                    arguments = Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0')
                    if str(root / 'work/config.yaml').encode() in arguments:
                        os.kill(pid, signal.SIGTERM)
            server.shutdown()
    print('All isolated stability checks passed; desktop proxy was never modified.')


if __name__ == '__main__':
    main()
