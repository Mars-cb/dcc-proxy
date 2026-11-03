#!/bin/bash
set -euo pipefail
: "${PACKAGE_VERSION:?}" "${RELEASE_TAG:?}" "${PRERELEASE:?}" "${GITHUB_SHA:?}"
assets="$(pwd)/release-assets"
cd "$assets"
# A Release is created only when both validated architectures are present.
for arch in amd64 arm64; do
    test -f "dcc-proxy_${PACKAGE_VERSION}_${arch}.deb"
    sha256sum --check --strict "SHA256SUMS-${arch}"
done
cat SHA256SUMS-amd64 SHA256SUMS-arm64 > SHA256SUMS
notes="$RUNNER_TEMP/dcc-proxy-release-notes.md"
cat > "$notes" <<EOF
自动构建版本：\`$PACKAGE_VERSION\`

源码提交：\`$GITHUB_SHA\`

构建及测试记录：$GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID

适用环境：UOS 20 / Qt 5.11 / DTK 5.6 控制中心。

- Intel/AMD 64 位电脑下载 \`amd64.deb\`，ARM64 电脑下载 \`arm64.deb\`。
- 内核、对应源码与基础地理数据库已随包提供；Qt/DTK 使用系统运行库。
- 安装示例：\`sudo apt install ./dcc-proxy_${PACKAGE_VERSION}_amd64.deb\`。
- 安装不会自动重启正在运行的代理后台或内核。
- 仅在两种架构的编译、回归测试、插件加载和包兼容性检查全部通过后发布。

每次 main 提交生成预发布包；匹配源码版本的 vX.Y.Z 标签生成正式版本。
EOF
options=(--target "$GITHUB_SHA" --title "dcc-proxy $PACKAGE_VERSION" --notes-file "$notes")
if [[ "$PRERELEASE" == true ]]; then options+=(--prerelease --latest=false); else options+=(--latest); fi
if gh release view "$RELEASE_TAG" > /dev/null 2>&1; then
    # Reruns of the same commit are safe; immutable tags are never moved.
    current="$(gh release view "$RELEASE_TAG" --json targetCommitish --jq .targetCommitish)"
    [[ "$current" == "$GITHUB_SHA" || "$current" == "${GITHUB_REF_NAME:-}" ]] || { echo 'Release target mismatch' >&2; exit 1; }
    gh release upload "$RELEASE_TAG" ./*.deb SHA256SUMS ./build-info-*.txt --clobber
else
    gh release create "$RELEASE_TAG" "${options[@]}" ./*.deb SHA256SUMS ./build-info-*.txt
fi
