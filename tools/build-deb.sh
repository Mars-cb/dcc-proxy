#!/bin/bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
package_arch="$(dpkg --print-architecture)"
build_dir="${project_dir}/build-${package_arch}"
core_version="$(cat "$project_dir/packaging/mihomo.version")"
cmake_args=(-S "$project_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr)
if [[ "${1:-}" != "--without-core" ]]; then
  "$project_dir/tools/prepare-core.sh" "$package_arch"
  cmake_args+=(-DMIHOMO_BINARY="$project_dir/cores/$package_arch/mihomo" -DMIHOMO_SOURCE_ARCHIVE="$project_dir/cores/mihomo-${core_version}-source.tar.gz")
else
  cmake_args+=(-DMIHOMO_BINARY= -DMIHOMO_SOURCE_ARCHIVE=)
fi
"$project_dir/tools/prepare-geodata.py"
cmake_args+=(-DMIHOMO_GEODATA_DIR="$project_dir/cores/geodata")
cmake "${cmake_args[@]}"
cmake --build "$build_dir" --parallel "${DCC_PROXY_JOBS:-2}"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$build_dir" --output-on-failure
(cd "$build_dir" && cpack -G DEB)
mkdir -p "$project_dir/dist"
cp "$build_dir"/*.deb "$project_dir/dist/"
echo "Package ready in $project_dir/dist/"
