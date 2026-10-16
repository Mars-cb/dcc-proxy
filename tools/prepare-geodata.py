#!/usr/bin/env python3
"""Build-time download/cache verification. Never installed or run by the deb."""
from pathlib import Path
import hashlib
import json
import os
import sys
import urllib.request

root = Path(__file__).resolve().parent.parent
manifest = json.loads((root / 'packaging/geodata.json').read_text())
cache = root / 'cores/geodata'
cache.mkdir(parents=True, exist_ok=True)
for item in manifest['files']:
    path = cache / item['file']
    if path.exists():
        data = path.read_bytes()
    else:
        if '--offline' in sys.argv:
            raise SystemExit('Missing cached database: ' + str(path))
        partial = path.with_suffix(path.suffix + '.part')
        with urllib.request.urlopen(item['url'], timeout=120) as response:
            data = response.read()
        partial.write_bytes(data)
        if hashlib.sha256(data).hexdigest() == item['sha256'] and len(data) == item['size']:
            os.replace(str(partial), str(path))
    if hashlib.sha256(data).hexdigest() != item['sha256'] or len(data) != item['size']:
        raise SystemExit('Official database checksum mismatch: ' + str(path))
    print('Verified bundled ' + item['file'])
(cache / 'geodata.json').write_text(json.dumps(manifest, indent=2) + '\n')
