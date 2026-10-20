#!/usr/bin/env python3
"""Build-time download/cache verification. Never installed or run by the deb."""
import argparse
import hashlib
import http.client
import json
import os
from pathlib import Path
import shutil
import sys
import time
import urllib.error
import urllib.request


def verify(path, item):
    digest = hashlib.sha256()
    size = 0
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(256 * 1024), b''):
            digest.update(chunk)
            size += len(chunk)
    return size == item['size'] and digest.hexdigest() == item['sha256']


def prepare_file(item, cache, offline=False, timeout=300, attempts=3):
    path = cache / item['file']
    if path.exists() and verify(path, item):
        print('Verified bundled ' + item['file'], flush=True)
        return
    if offline:
        raise RuntimeError('Missing or invalid cached database: ' + str(path))

    partial = path.with_suffix(path.suffix + '.part')
    for attempt in range(1, attempts + 1):
        print('Downloading {} (attempt {}/{}, timeout {}s): {}'.format(
            item['file'], attempt, attempts, timeout, item['url']), flush=True)
        try:
            with urllib.request.urlopen(item['url'], timeout=timeout) as response:
                with partial.open('wb') as output:
                    shutil.copyfileobj(response, output, length=256 * 1024)
            if not verify(partial, item):
                raise ValueError('Official database size or SHA-256 mismatch')
            os.replace(str(partial), str(path))
        except (OSError, urllib.error.URLError, http.client.HTTPException,
                ValueError) as error:
            print('Download failed for {}: {}'.format(item['file'], error),
                  file=sys.stderr, flush=True)
            if attempt == attempts:
                raise RuntimeError(
                    'Unable to prepare {} after {} attempts. Check access to {} '
                    'or HTTPS_PROXY, then rerun tools/prepare-geodata.py. '
                    'For offline builds, place the files listed in '
                    'packaging/geodata.json in {} and verify with '
                    'tools/prepare-geodata.py --offline.'.format(
                        item['file'], attempts, item['url'], cache)) from error
        else:
            print('Verified bundled ' + item['file'], flush=True)
            return
        finally:
            if partial.exists():
                partial.unlink()
        time.sleep(2 ** attempt)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true',
                        help='verify cached files without downloading')
    parser.add_argument('--timeout', type=float,
                        default=os.environ.get('DCC_PROXY_GEODATA_TIMEOUT', '300'),
                        help='socket timeout in seconds (default: 300; '
                             'env: DCC_PROXY_GEODATA_TIMEOUT)')
    parser.add_argument('--attempts', type=int,
                        default=os.environ.get('DCC_PROXY_GEODATA_ATTEMPTS', '3'),
                        help='maximum download attempts per file (default: 3; '
                             'env: DCC_PROXY_GEODATA_ATTEMPTS)')
    args = parser.parse_args()
    if not 0 < args.timeout < float('inf') or args.attempts < 1:
        parser.error('--timeout must be finite and positive; --attempts must be at least 1')

    root = Path(__file__).resolve().parent.parent
    manifest = json.loads((root / 'packaging/geodata.json').read_text())
    cache = root / 'cores/geodata'
    cache.mkdir(parents=True, exist_ok=True)
    try:
        for item in manifest['files']:
            prepare_file(item, cache, args.offline, args.timeout, args.attempts)
    except RuntimeError as error:
        parser.exit(1, str(error) + '\n')
    (cache / 'geodata.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
