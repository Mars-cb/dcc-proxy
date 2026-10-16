#!/usr/bin/env python3
"""Prove rule/global/direct routing using a real core and local CONNECT relay.

Only temporary data and random loopback ports are used; no desktop proxy changes.
"""
import argparse
import contextlib
import http.client
import http.server
import json
import os
from pathlib import Path
import secrets
import select
import socket
import subprocess
import tempfile
import threading
import time
import urllib.parse
import urllib.request


class Origin(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.hits += 1
        body = b'mode-routing-origin'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *_): pass


class Relay(http.server.BaseHTTPRequestHandler):
    def do_CONNECT(self):
        host, port = self.path.rsplit(':', 1)
        if host != '127.0.0.1' or int(port) not in self.server.allowed_ports:
            self.send_error(403); return
        with socket.create_connection((host, int(port)), timeout=3) as upstream:
            self.server.connects += 1
            self.send_response(200, 'Connection Established')
            self.end_headers()
            self.wfile.flush()
            while True:
                readable, _, _ = select.select([self.connection, upstream], [], [], 4)
                if not readable: return
                for source in readable:
                    data = source.recv(65536)
                    if not data: return
                    (upstream if source is self.connection else self.connection).sendall(data)
    def log_message(self, *_): pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--service', default='/usr/lib/dcc-proxy/dde-mihomo-daemon')
    parser.add_argument('--core', default='/usr/lib/dcc-proxy/mihomo')
    args = parser.parse_args()
    service, core = str(Path(args.service).resolve()), str(Path(args.core).resolve())
    servers = []
    for _ in range(3):
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Origin)
        server.hits = 0; servers.append(server)
    relay = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Relay)
    relay.connects = 0; relay.allowed_ports = {server.server_port for server in servers}
    for server in servers + [relay]:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='dcc-proxy-modes-') as tmp:
            root = Path(tmp); (root/'profiles').mkdir()
            with socket.socket() as first, socket.socket() as second:
                first.bind(('127.0.0.1', 0)); second.bind(('127.0.0.1', 0))
                controller, mixed = first.getsockname()[1], second.getsockname()[1]
            profile, token = '00000000-0000-4000-8000-000000000006', secrets.token_hex(24)
            config = ('mixed-port: %d\nmode: rule\nlog-level: info\n'
                'proxies:\n  - name: TEST_PROXY\n    type: http\n    server: 127.0.0.1\n    port: %d\n'
                'proxy-groups:\n  - name: POLICY\n    type: select\n    proxies: [TEST_PROXY, DIRECT, REJECT]\n'
                'rules:\n  - DST-PORT,%d,DIRECT\n  - DST-PORT,%d,REJECT\n  - MATCH,POLICY\n') % (
                    mixed, relay.server_port, servers[1].server_port, servers[2].server_port)
            (root/'profiles'/(profile+'.yaml')).write_text(config)
            (root/'profiles.json').write_text(json.dumps({'profiles':[{'id':profile,'name':'mode fixture','url':''}]}))
            (root/'settings.json').write_text(json.dumps({'endpoint':'http://127.0.0.1:%d'%controller,
                'secret':token,'core':core,'active':profile,'autoStart':False,'autoSystemProxy':False,'updateMinutes':0}))
            env = dict(os.environ, DCC_PROXY_DATA_DIR=tmp,
                DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent', DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent')
            daemon = subprocess.Popen([service,'--daemon'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            def api(path, method='GET', data=None):
                req = urllib.request.Request('http://127.0.0.1:%d%s'%(controller,path),
                    data=None if data is None else json.dumps(data).encode(), method=method,
                    headers={'Authorization':'Bearer '+token,'Content-Type':'application/json'})
                with opener.open(req,timeout=4) as response:
                    raw=response.read();return json.loads(raw) if raw else {}
            def choose(group, node):
                path='/proxies/'+urllib.parse.quote(group,safe='')
                api(path,'PUT',{'name':node});assert api(path)['now']==node
            def mode(value):
                api('/configs','PATCH',{'mode':value});assert api('/configs')['mode']==value
            def flow(index, expected):
                origin=servers[index];hits,connects=origin.hits,relay.connects
                conn=http.client.HTTPConnection('127.0.0.1',mixed,timeout=4)
                body=None
                try:
                    conn.request('GET','http://127.0.0.1:%d/test'%origin.server_port,headers={'Connection':'close'})
                    body=conn.getresponse().read()
                except (OSError,http.client.HTTPException):
                    if expected!='reject':raise
                finally:conn.close()
                if expected=='reject':
                    assert body!=b'mode-routing-origin' and origin.hits==hits and relay.connects==connects,(index,expected,body)
                else:
                    assert body==b'mode-routing-origin' and origin.hits==hits+1,(index,expected,body)
                    assert relay.connects-connects==(1 if expected=='proxy' else 0),(index,expected,relay.connects-connects)
            try:
                for _ in range(100):
                    if (root/'service.sock').exists():break
                    assert daemon.poll() is None,'Backend exited early';time.sleep(.05)
                result=json.loads(subprocess.check_output([service,'--command','start'],env=env,text=True,timeout=30))
                assert result.get('running') and not result.get('error'),result
                choose('POLICY','TEST_PROXY');choose('GLOBAL','REJECT');mode('rule')
                flow(0,'proxy');flow(1,'direct');flow(2,'reject')
                choose('POLICY','REJECT');flow(0,'reject');flow(1,'direct')
                print('PASS rule: matching rules choose proxy, DIRECT and REJECT independently; POLICY changes affect only its rule',flush=True)
                choose('POLICY','REJECT');choose('GLOBAL','TEST_PROXY');mode('global')
                for index in range(3):flow(index,'proxy')
                choose('GLOBAL','DIRECT');flow(0,'direct');flow(2,'direct')
                choose('GLOBAL','REJECT');flow(0,'reject');flow(1,'reject')
                print('PASS global: all destinations follow GLOBAL selection, overriding DIRECT/REJECT rules and POLICY selection',flush=True)
                choose('GLOBAL','REJECT');choose('POLICY','REJECT');mode('direct')
                for index in range(3):flow(index,'direct')
                print('PASS direct: all destinations reach origin without relay despite REJECT rules and group selections',flush=True)
                mode('rule');choose('POLICY','TEST_PROXY');flow(0,'proxy');flow(1,'direct');flow(2,'reject')
                print('PASS switching back restores rule routing; tests use fresh connections and isolated ports',flush=True)
            except Exception:
                if (root/'daemon.log').exists():print((root/'daemon.log').read_text()[-6000:])
                raise
            finally:
                daemon.terminate()
                try:daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:daemon.kill();daemon.wait(timeout=5)
    finally:
        for server in servers+[relay]:server.shutdown();server.server_close()


if __name__=='__main__':main()
