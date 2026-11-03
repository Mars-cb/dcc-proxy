#!/bin/bash
set -euo pipefail
. /etc/os-release
[[ "$ID" == debian && "$VERSION_ID" == 10 ]] || { echo 'Run only inside the Debian 10 CI builder.' >&2; exit 1; }
ci_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
builder_arch="$(dpkg --print-architecture)"
build_root="${DCC_DTK_BUILD_ROOT:-/tmp/dcc-proxy-dtk}"
mkdir -p "$build_root"
while read -r component version checksum; do
    [[ -z "$component" || "$component" == \#* ]] && continue
    archive="$build_root/$component-$version.tar.gz"
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 --connect-timeout 20 --max-time 600 \
            "https://codeload.github.com/linuxdeepin/$component/tar.gz/refs/tags/$version" -o "$archive"
    fi
    printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --strict
    source_dir="$build_root/$component-$version"
    mkdir -p "$source_dir"
    tar -xf "$archive" --strip-components=1 --no-same-owner -C "$source_dir"
    cmake -S "$source_dir" -B "$source_dir/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
        -DDTK_VERSION="$version" -DDVERSION="$version" -DVERSION="$version" \
        -DBUILD_DOCS=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DBUILD_PLUGINS=OFF \
        -DDTK_DISABLE_LIBXDG=ON -DDTK_DISABLE_EX_IMAGE_FORMAT=ON
    cmake --build "$source_dir/build" --parallel "${DCC_PROXY_JOBS:-2}"
    stage="$source_dir/stage"
    mkdir -p "$stage/DEBIAN"
    DESTDIR="$stage" cmake --build "$source_dir/build" --target install
    if [[ "$component" == dtkcommon ]]; then package=libdtkcommon-dev; else package="lib${component}5"; fi
    cat > "$stage/DEBIAN/control" <<EOF
Package: $package
Version: $version-ci1
Architecture: $builder_arch
Maintainer: DCC Proxy maintainers <Mars-cb@users.noreply.github.com>
Description: Fixed DTK SDK for the dcc-proxy CI builder only
EOF
    # Register ownership/shlibs so CPack emits real system DTK dependencies.
    # These SDK packages stay in the builder; they are never bundled in our deb.
    if [[ "$component" != dtkcommon ]]; then
        printf 'lib%s 5 %s (>= %s)\n' "$component" "$package" "$version" > "$stage/DEBIAN/shlibs"
    fi
    dpkg-deb --build "$stage" "$source_dir/sdk.deb"
    dpkg -i "$source_dir/sdk.deb"
    ldconfig
done < "$ci_dir/dtk-sources.tsv"
glib-compile-schemas /usr/share/glib-2.0/schemas
