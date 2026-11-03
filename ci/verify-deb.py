#!/usr/bin/env python3
"""Reject wrong versions/architectures and newer-than-UOS20 runtime requirements."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def verify(package, version, arch):
    for field, expected in [('Package', 'dcc-proxy'), ('Version', version), ('Architecture', arch)]:
        actual = command('dpkg-deb', '-f', str(package), field)
        if actual != expected:
            raise ValueError('{}: {} is {}, expected {}'.format(package, field, actual, expected))
    dependencies = command('dpkg-deb', '-f', str(package), 'Depends')
    for dependency in ('dde-control-center', 'libdtkcore5', 'libdtkgui5', 'libdtkwidget5', 'libqt5widgets5'):
        if dependency not in dependencies:
            raise ValueError('Missing system dependency: ' + dependency)
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        subprocess.check_call(['dpkg-deb', '-x', str(package), directory])
        plugin = root / 'usr/lib/dde-control-center/modules/libdcc-proxy.so'
        daemon = root / 'usr/lib/dcc-proxy/dde-mihomo-daemon'
        core = root / 'usr/lib/dcc-proxy/mihomo'
        for path in (plugin, daemon, core):
            if not path.is_file():
                raise ValueError('Missing binary: ' + str(path.relative_to(root)))
            header = command('readelf', '-h', str(path))
            machine = 'Advanced Micro Devices X86-64' if arch == 'amd64' else 'AArch64'
            if machine not in header:
                raise ValueError('Unexpected ELF architecture: ' + str(path))
        for path in (plugin, daemon):
            symbols = command('readelf', '--version-info', str(path))
            for prefix, maximum in [('GLIBC', (2, 28)), ('GLIBCXX', (3, 4, 25))]:
                for value in re.findall(prefix + r'_([0-9]+(?:\.[0-9]+)+)', symbols):
                    if tuple(map(int, value.split('.'))) > maximum:
                        raise ValueError('{} requires {}_{} beyond the UOS20 baseline'.format(path.name, prefix, value))
        if any(path.name.startswith(('libdtk', 'libQt5')) for path in root.rglob('*.so*')):
            raise ValueError('Do not bundle system DTK/Qt libraries')
        for name in ('Country.mmdb', 'GeoIP.dat', 'GeoSite.dat', 'ASN.mmdb'):
            if not (root / 'usr/share/dcc-proxy' / name).is_file():
                raise ValueError('Missing offline geodata: ' + name)
        if not (root / 'usr/share/doc/dcc-proxy/mihomo-source.tar.gz').is_file():
            raise ValueError('Missing corresponding core source archive')
    print('Verified package:', package.name)
    print('Runtime dependencies:', dependencies)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--arch', required=True, choices=['amd64', 'arm64'])
    args = parser.parse_args()
    try:
        verify(args.package, args.version, args.arch)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
