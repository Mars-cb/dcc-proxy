#!/usr/bin/env python3
"""One checked version for CMake, Debian packages and GitHub Releases."""
import argparse
import re
from pathlib import Path


def base_version(root):
    cmake = re.search(r'project\(dcc-proxy\s+VERSION\s+(\d+\.\d+\.\d+)\s', (root / 'CMakeLists.txt').read_text())
    debian = re.match(r'dcc-proxy \(([^)]+)\)', (root / 'debian/changelog').read_text())
    if not cmake or not debian or cmake.group(1) != debian.group(1):
        raise ValueError('CMake and debian/changelog versions must agree')
    return cmake.group(1)


def release_metadata(base, ref, run_number, commit):
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected a full commit SHA')
    if ref.startswith('refs/tags/'):
        tag = ref[len('refs/tags/'):]
        if tag != 'v' + base:
            raise ValueError('Release tag must match the source version: v' + base)
        return {'version': base, 'release_tag': tag, 'prerelease': 'false'}
    if not re.fullmatch(r'[1-9][0-9]*', run_number):
        raise ValueError('Expected a positive workflow run number')
    # Previews upgrade in run order, but remain lower than the eventual stable release.
    return {'version': '{}~ci.{}.g{}'.format(base, run_number, commit[:7]),
            'release_tag': 'ci-{}-{}'.format(run_number, commit[:7]), 'prerelease': 'true'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', action='store_true')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--ref', default='refs/heads/main')
    parser.add_argument('--run-number', default='')
    parser.add_argument('--commit', default='')
    parser.add_argument('--github-output', type=Path)
    args = parser.parse_args()
    try:
        base = base_version(args.root)
        if args.base:
            print(base)
            return
        values = release_metadata(base, args.ref, args.run_number, args.commit)
        values['base_version'] = base
        output = ''.join('{}={}\n'.format(key, value) for key, value in values.items())
        if args.github_output:
            with args.github_output.open('a') as stream:
                stream.write(output)
        print(output, end='')
    except ValueError as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
