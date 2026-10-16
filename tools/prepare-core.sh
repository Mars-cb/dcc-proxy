#!/bin/bash
# Download and verify only the official core matching the requested host architecture.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
package_arch="${1:-$(dpkg --print-architecture)}"
core_version="$(cat "$project_dir/packaging/mihomo.version")"
case "$package_arch" in
  amd64) core_arch=amd64-compatible; expected_core=a3c0d07cee456d84d22855c201d1278d8989ad1e43935cf252511b5ac8599a6b ;;
  arm64) core_arch=arm64; expected_core=452d1fc046da65b5684fcf3d102d59acb8a6c2ff546d93afa669255e1c511820 ;;
  *) echo "No configured official core for $package_arch; use DCC_PROXY_BUNDLE_CORE=0 with dpkg-buildpackage, or tools/build-deb.sh --without-core." >&2
     exit 1 ;;
esac
core_dir="$project_dir/cores/$package_arch"
mkdir -p "$core_dir"
asset="mihomo-linux-${core_arch}-${core_version}.gz"
if [[ ! -f "$core_dir/mihomo" ]]; then
  curl --fail --location --proto '=https' --connect-timeout 15 --max-time 300 \
    "https://github.com/MetaCubeX/mihomo/releases/download/${core_version}/${asset}" -o "$core_dir/core.gz.part"
  gzip -dc "$core_dir/core.gz.part" > "$core_dir/mihomo.part"
  chmod 755 "$core_dir/mihomo.part"
  mv "$core_dir/mihomo.part" "$core_dir/mihomo"
  mv "$core_dir/core.gz.part" "$core_dir/$asset"
fi
actual_core="$(sha256sum "$core_dir/mihomo")"
if [[ "${actual_core%% *}" != "$expected_core" ]]; then
  echo "Mihomo checksum mismatch. Remove the cached binary and retry the official download." >&2
  exit 1
fi
if [[ "$package_arch" == "$(dpkg --print-architecture)" ]]; then
  "$core_dir/mihomo" -v
fi
source_archive="$project_dir/cores/mihomo-${core_version}-source.tar.gz"
if [[ ! -f "$source_archive" ]]; then
  curl --fail --location --proto '=https' --connect-timeout 15 --max-time 300 \
    "https://codeload.github.com/MetaCubeX/mihomo/tar.gz/refs/tags/${core_version}" -o "$source_archive.part"
  tar -tzf "$source_archive.part" > /dev/null
  mv "$source_archive.part" "$source_archive"
fi
actual_source="$(sha256sum "$source_archive")"
if [[ "${actual_source%% *}" != 9f2d029f7d074cb2f0f9c7bc59f47fddf48bd9ce2ce3532cd91d00fd89ee25f7 ]]; then
  echo "Mihomo source archive checksum mismatch." >&2
  exit 1
fi
(cd "$core_dir" && sha256sum mihomo > SHA256SUMS)
