#!/bin/bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
package_arch="$(dpkg --print-architecture)"
build_dir="${project_dir}/build-${package_arch}"
core_version=v1.19.12
bundle_core=ON
if [[ "${1:-}" == "--without-core" ]]; then bundle_core=OFF; fi
case "$package_arch" in
  amd64) core_arch=amd64-compatible; expected_core=a3c0d07cee456d84d22855c201d1278d8989ad1e43935cf252511b5ac8599a6b ;;
  arm64) core_arch=arm64; expected_core=452d1fc046da65b5684fcf3d102d59acb8a6c2ff546d93afa669255e1c511820 ;;
  *) echo "No configured official core for $package_arch; use --without-core and select an existing core." >&2
     if [[ "$bundle_core" == ON ]]; then exit 1; fi
     core_arch=unused ;;
esac
cmake_args=(-S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr)
if [[ "$bundle_core" == ON ]]; then
  core_dir="$project_dir/cores/$package_arch"
  mkdir -p "$core_dir"
  asset="mihomo-linux-${core_arch}-${core_version}.gz"
  if [[ ! -f "$core_dir/mihomo" ]]; then
    curl --fail --location --proto '=https' --connect-timeout 15 --max-time 300 \
      "https://github.com/MetaCubeX/mihomo/releases/download/${core_version}/${asset}" -o "$core_dir/core.gz.part"
    gzip -dc "$core_dir/core.gz.part" > "$core_dir/mihomo.part"
    chmod 755 "$core_dir/mihomo.part"
    "$core_dir/mihomo.part" -v
    mv "$core_dir/mihomo.part" "$core_dir/mihomo"
    mv "$core_dir/core.gz.part" "$core_dir/$asset"
  fi
  actual_core="$(sha256sum "$core_dir/mihomo")"
  if [[ "${actual_core%% *}" != "$expected_core" ]]; then
    echo "Mihomo checksum mismatch. Remove the cached binary and retry the official download." >&2
    exit 1
  fi
  "$core_dir/mihomo" -v
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
  cmake_args+=(-DMIHOMO_BINARY="$core_dir/mihomo" -DMIHOMO_SOURCE_ARCHIVE="$source_archive")
else
  cmake_args+=(-DMIHOMO_BINARY= -DMIHOMO_SOURCE_ARCHIVE=)
fi
cmake "${cmake_args[@]}"
cmake --build "$build_dir" --parallel "${DCC_PROXY_JOBS:-2}"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" --output-on-failure
(cd "$build_dir" && cpack -G DEB)
mkdir -p "$project_dir/dist"
cp "$build_dir"/*.deb "$project_dir/dist/"
echo "Package ready in $project_dir/dist/"
