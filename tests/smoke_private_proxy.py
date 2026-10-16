#!/usr/bin/env python3
"""Run desktop proxy checks on a private session bus, using existing DBus/GLib."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--service', required=True)
    parser.add_argument('--core', default='/usr/lib/dcc-proxy/mihomo')
    parser.add_argument('--private', action='store_true')
    parser.add_argument('--host-bus')
    args = parser.parse_args()
    if not args.private:
        env = dict(os.environ, GSETTINGS_BACKEND='memory', DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent')
        with tempfile.TemporaryDirectory(prefix='dcc-proxy-private-bus-') as tmp:
            config = Path(tmp) / 'bus.conf'
            config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
                '<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/>'
                '<allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
            return subprocess.call(['dbus-run-session', '--config-file', str(config), '--', sys.executable, __file__, '--private',
                '--host-bus', os.environ.get('DBUS_SESSION_BUS_ADDRESS', ''),
                '--service', str(Path(args.service).resolve()), '--core', str(Path(args.core).resolve())], env=env)
    assert os.environ['DBUS_SESSION_BUS_ADDRESS'] != args.host_bus, 'Refusing to use desktop bus'
    assert os.environ.get('GSETTINGS_BACKEND') == 'memory', 'Refusing persistent desktop settings'
    import dbus
    import dbus.service
    import dbus.mainloop.glib
    from gi.repository import GLib
    import threading
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    interface = 'org.deepin.dde.Network1'
    name = dbus.service.BusName(interface, bus)

    class Network(dbus.service.Object):
        def __init__(self):
            super().__init__(name, '/org/deepin/dde/Network1')
            self.mode, self.pac, self.ignore = 'none', '', 'localhost'
            self.proxies = {kind: ('192.0.2.1', '3128') for kind in ('http', 'https', 'socks', 'ftp')}
        @dbus.service.method(interface, in_signature='', out_signature='s')
        def GetProxyMethod(self): return self.mode
        @dbus.service.method(interface, in_signature='s', out_signature='')
        def SetProxyMethod(self, value): self.mode = str(value)
        @dbus.service.method(interface, in_signature='', out_signature='s')
        def GetAutoProxy(self): return self.pac
        @dbus.service.method(interface, in_signature='s', out_signature='')
        def SetAutoProxy(self, value): self.pac = str(value)
        @dbus.service.method(interface, in_signature='', out_signature='s')
        def GetProxyIgnoreHosts(self): return self.ignore
        @dbus.service.method(interface, in_signature='s', out_signature='')
        def SetProxyIgnoreHosts(self, value): self.ignore = str(value)
        @dbus.service.method(interface, in_signature='s', out_signature='ss')
        def GetProxy(self, kind): return self.proxies[str(kind)]
        @dbus.service.method(interface, in_signature='sss', out_signature='')
        def SetProxy(self, kind, host, port): self.proxies[str(kind)] = (str(host), str(port))
    network = Network()
    loop = GLib.MainLoop(); results = []
    def test():
        try:
            results.append(subprocess.call([sys.executable, str(Path(__file__).with_name('smoke_system_proxy.py')),
                '--service', args.service, '--core', args.core, '--allow-system-proxy']))
        finally: GLib.idle_add(loop.quit)
    thread = threading.Thread(target=test); thread.start(); loop.run(); thread.join()
    return results[0]


if __name__ == '__main__':
    raise SystemExit(main())
