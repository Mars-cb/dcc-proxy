#!/usr/bin/env python3
"""Verify all bundled geography resources with real Mihomo and no external network."""
import argparse
from pathlib import Path
import hashlib
import http.client
import http.server
import json
import os
import secrets
import socket
import subprocess
import tempfile
import threading
import time

class Target(http.server.BaseHTTPRequestHandler):
    downloads = 0
    def do_GET(self):
        if self.path.startswith('/download'):
            Target.downloads += 1; self.send_error(503); return
        body = b'dcc-proxy-offline-geodata-ok'
        self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *_): pass

def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); return sock.getsockname()[1]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host-netns', required=True)
    parser.add_argument('--service', default='/usr/lib/dcc-proxy/dde-mihomo-daemon')
    parser.add_argument('--core', default='/usr/lib/dcc-proxy/mihomo')
    parser.add_argument('--resources', default='/usr/share/dcc-proxy')
    parser.add_argument('--existing-snapshot')
    args = parser.parse_args()
    assert os.readlink('/proc/self/ns/net') != args.host_netns, 'Require a separate network namespace'
    assert {entry['ifname'] for entry in json.loads(subprocess.check_output(['ip','-j','link'],text=True))} == {'lo'}, 'Require a fresh offline namespace'
    subprocess.run(['ip','link','set','lo','up'],check=True)
    resources=Path(args.resources).resolve();manifest=json.loads((resources/'geodata.json').read_text())
    target=http.server.HTTPServer(('127.0.0.1',0),Target);threading.Thread(target=target.serve_forever,daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='dcc-proxy-offline-geodata-') as tmp:
        root=Path(tmp);(root/'profiles').mkdir();profile='00000000-0000-4000-8000-000000000003'
        controller,mixed,token=port(),port(),secrets.token_hex(24)
        config='mixed-port: %d\nmode: rule\nlog-level: info\ngeo-auto-update: true\ngeox-url:\n'%mixed
        for name in ('mmdb','geoip','geosite','asn'):config+='  %s: http://127.0.0.1:%d/download/%s\n'%(name,target.server_port,name)
        config+='rules:\n  - GEOSITE,cn,DIRECT\n  - GEOIP,CN,DIRECT,no-resolve\n  - IP-ASN,4134,DIRECT,no-resolve\n  - MATCH,DIRECT\n'
        profile_path=root/'profiles'/(profile+'.yaml');profile_path.write_text(config)
        (root/'settings.json').write_text(json.dumps({'endpoint':'http://127.0.0.1:%d'%controller,'secret':token,'core':str(Path(args.core).resolve()),'autoStart':False,'autoSystemProxy':False,'updateMinutes':0}))
        (root/'profiles.json').write_text(json.dumps({'profiles':[{'id':profile,'name':'offline GeoIP+GeoSite+ASN fixture','url':''}]}))
        env=dict(os.environ,DCC_PROXY_DATA_DIR=tmp,DCC_PROXY_GEODATA_DIR=str(resources),DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent',DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent')
        daemon=subprocess.Popen([str(Path(args.service).resolve()),'--daemon'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        def command(action):
            with socket.socket(socket.AF_UNIX) as sock:
                sock.settimeout(30);sock.connect(str(root/'service.sock'));sock.sendall(json.dumps({'action':action}).encode()+b'\n');data=b''
                while b'\n' not in data:
                    part=sock.recv(65536)
                    if not part:raise RuntimeError('Daemon closed connection')
                    data+=part
                return json.loads(data.split(b'\n')[0])
        def start():
            before=time.monotonic();result=command('start')
            assert result.get('running') and not result.get('error'),result
            assert time.monotonic()-before<20,'Offline initial startup unexpectedly waited for downloads'
            assert Target.downloads==0,'The core attempted an online database download'
            conn=http.client.HTTPConnection('127.0.0.1',mixed,timeout=5)
            conn.request('GET','http://127.0.0.1:%d/'%target.server_port);response=conn.getresponse()
            assert response.status==200 and response.read()==b'dcc-proxy-offline-geodata-ok';conn.close()
            assert 'geo-auto-update: false' in (root/'work/config.yaml').read_text()
        try:
            for _ in range(50):
                if (root/'service.sock').exists():break
                time.sleep(.1)
            start();work=root/'work'
            for item in manifest['files']:
                file=work/item['file'];assert hashlib.sha256(file.read_bytes()).hexdigest()==item['sha256']
                assert file.stat().st_mode&0o777==0o600
            print('PASS: fresh empty cache starts offline with real GEOIP MMDB, GEOSITE and IP-ASN rules; no download requests',flush=True)
            command('stop')
            for item in manifest['files']:
                file=work/item['file']
                if file.suffix=='.mmdb':file.write_bytes(b'interrupted database download')
                else:file.write_bytes(file.read_bytes()[:-7])
            profile_path.write_text('geodata-mode: true\n'+config)
            start()
            for item in manifest['files']:
                file=work/item['file'];assert hashlib.sha256(file.read_bytes()).hexdigest()==item['sha256']
                assert list(work.glob(item['file']+'.invalid-*'))
            print('PASS: interrupted MMDB/protobuf caches backed up and recovered; GeoIP.dat mode also starts offline',flush=True)
            command('stop')
            if args.existing_snapshot:
                existing=Path(args.existing_snapshot).read_bytes();(work/'Country.mmdb').write_bytes(existing)
                profile_path.write_text(config);start()
                assert (work/'Country.mmdb').read_bytes()==existing
                print('PASS: existing usable older user database preserved',flush=True)
                command('stop')
            (work/'Country.mmdb').rename(work/'geoip.metadb');start()
            assert not (work/'Country.mmdb').exists() and (work/'geoip.metadb').exists()
            print('PASS: existing Mihomo database alias preserved without network',flush=True)
            command('stop')
        except Exception:
            log=root/'daemon.log'
            if log.exists():print(log.read_text()[-12000:])
            raise
        finally:
            daemon.terminate()
            try:daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:daemon.kill();daemon.wait()
            target.shutdown()
if __name__=='__main__':main()
