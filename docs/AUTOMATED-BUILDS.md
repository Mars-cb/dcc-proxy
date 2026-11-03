# 云端自动打包

仓库使用 GitHub 官方 AMD64 / ARM64 Runner，全程不需要自托管机器或 UOS 内网软件源。
工作流位于 `.github/workflows/build-release.yml`，仅发布编译、回归和包检查全部通过的产物。

## 下载与触发

- 推送 `main`：两种架构均成功后，在 Releases 发布 `ci-运行编号-提交短号` 预发布。
- 推送 `vX.Y.Z`：标签必须与 CMake / Debian changelog 的版本一致，发布正式 Release。
- Actions 的 Run workflow 按钮可手动重新构建；只有 main 或版本标签上的运行才发布。
- PR 和 `ci/**` 分支：构建与测试，保留 Actions artifacts，不发布 Release。
- 每个不同提交独立构建；同一个 ref/commit 重复触发时仅保留最新运行。

预发布 deb 版本示例：`0.1.12~ci.123.g0123456`。其排序高于 `0.1.11`，低于未来的正式 `0.1.12`。
正式标签 `v0.1.12` 则生成 `0.1.12`。AMD64 / ARM64 产物使用相同版本；Release 附带
SHA256SUMS 与每种架构的构建来源记录。不能把不同源码提交的产物混入同一个 Release。

用户下载对应架构的 deb 后执行：

```sh
sudo apt install ./dcc-proxy_版本_amd64.deb
```

Qt、DTK 和控制中心仍由系统提供；内核、对应源码和四个基础地理数据库随包提供。
安装过程不下载内核或数据库，不自动重启现有代理后台和内核。

## 构建兼容环境

`ci/Dockerfile` 固定官方 Debian 10 多架构镜像摘要，其 glibc 2.28 / Qt 5.11 基线与目标 UOS 20 对齐。
APT 使用公开的 Debian 10 归档源及归档安全更新源。DTK common 5.6.4、Core 5.6.13、Gui 5.6.9、
Widget 5.6.13 从 linuxdeepin 官方源码构建，并在解压前验证 `ci/dtk-sources.tsv` 的 SHA-256。
选择互相兼容且不高于目标系统的 SDK：目标 UOS 的 Widget 5.6.18 能满足所需运行库版本，
不会为适配公开源码中的新接口而要求升级系统 Gui 库。
构建镜像只用于编译，最终 deb 不捆绑这些 DTK/Qt 动态库，也不替换系统 DTK。

DTK 在构建环境中登记库的包归属及 shlibs，确保 CPack 能生成真正的系统依赖声明。
控制中心使用仓库内的目标接口快照：已与目标 5.10.0 SDK 核对，接口签名/布局和 IID 1.0 一致，
仅注释和 IID 宏表达方式不同。`ci/dcc-interface.sha256` 固定快照，独立插件加载测试验证 IID 与动态链接。
接口快照发生变化时，必须重新核对目标 SDK，再更新校验文件，不能只跳过检查。

构建镜像使用 GitHub Actions 缓存，内核和数据库缓存仍会逐次校验。下载缺失或校验失败会阻止发布。
两种架构分别在原生 Runner 上编译和测试，不用 QEMU 代替运行时回归。
Actions 本身运行在现代宿主机，只有项目编译步骤进入兼容容器。

## 发布前检查

1. CMake / Debian changelog 版本一致，正式标签与源码版本一致。
2. 完整 Qt 回归、真实插件动态加载、版本排序和 GeoData 模拟下载回归通过。
3. deb 名称、内部包名、版本、架构及 ELF 架构正确。
4. 必需的系统依赖、内核、对应源码和离线数据库齐全。
5. 插件与后台不要求高于 GLIBC 2.28 / GLIBCXX 3.4.25 的运行库，deb 不携带私有 DTK/Qt 库。
6. 解压 deb，加载其中的插件，并用其中的后台、内核和数据库执行真实 HTTP 代理、更新及失败回滚冒烟测试。
7. 两种架构均上传成功并通过 SHA-256 校验后，由独立 Release job 发布。

构建 job 只有仓库读取权限；Release job 才拥有 `contents: write`，使用 GitHub 自动提供的
`GITHUB_TOKEN`，不需要个人 PAT。PR 不进入发布 job。固定的 CI 镜像准备脚本会拒绝在 UOS 宿主系统运行。

## 本地构建保持可用

用户直接 clone 后仍可运行 `dpkg-buildpackage -us -uc -b` 或 `./tools/build-deb.sh`，不需要 Docker 或 Actions。
默认本地版本为 CMake 中的正常版本号；CI 通过 `DCC_PROXY_PACKAGE_VERSION` 传入构建版本。
需要手动覆盖时，CPack 脚本也支持该环境变量。修改正式版本时，同时更新 CMake 与 Debian changelog。
