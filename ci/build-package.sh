#!/bin/bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p "${HOME:-/tmp/ci-home}"
export QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR=/tmp/dcc-proxy-ci-runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
builder_arch="$(dpkg --print-architecture)"
: "${DCC_PROXY_PACKAGE_VERSION:?CI package version is required}"
# Use the reviewed target interface snapshot instead of an unrelated host SDK.
sha256sum --check --strict ci/dcc-interface.sha256
python3 -m unittest discover -s tests -p 'test_ci_version.py'
python3 tests/test_prepare_geodata.py
bash tools/build-deb.sh
package="dist/dcc-proxy_${DCC_PROXY_PACKAGE_VERSION}_${builder_arch}.deb"
python3 ci/verify-deb.py "$package" --version "$DCC_PROXY_PACKAGE_VERSION" --arch "$builder_arch"
package_root="$(mktemp -d /tmp/dcc-proxy-package-XXXXXX)"
trap 'rm -rf -- "$package_root"' EXIT
dpkg-deb -x "$package" "$package_root"
cmp "$package_root/usr/lib/dcc-proxy/mihomo" "cores/$builder_arch/mihomo"
export DCC_PROXY_GEODATA_DIR="$package_root/usr/share/dcc-proxy"
"build-$builder_arch/party-plugin-smoke" "$package_root/usr/lib/dde-control-center/modules/libdcc-proxy.so"
# Exercise binaries from the deb, not merely the build tree. Each script uses
# temporary stores/ports and cannot access the desktop's real proxy settings.
python3 tests/smoke_real_core.py --service "$package_root/usr/lib/dcc-proxy/dde-mihomo-daemon" --core "$package_root/usr/lib/dcc-proxy/mihomo"
python3 tests/smoke_stability.py --service "$package_root/usr/lib/dcc-proxy/dde-mihomo-daemon" --core "$package_root/usr/lib/dcc-proxy/mihomo"
printf 'version=%s\ncommit=%s\narchitecture=%s\nqt=%s\n' \
    "$DCC_PROXY_PACKAGE_VERSION" "${GITHUB_SHA:-unknown}" "$builder_arch" "$(qmake -query QT_VERSION)" > "dist/build-info-${builder_arch}.txt"
cat ci/dtk-sources.tsv >> "dist/build-info-${builder_arch}.txt"
(cd dist && sha256sum "$(basename "$package")" > "SHA256SUMS-${builder_arch}")
