# dcc-proxy

UOS / Deepin 控制中心的原生 Qt 5 私人代理插件。插件包名和模块名均为 `dcc-proxy`，
后台进程名为 `dde-mihomo-daemon`。关闭控制中心后，后台内核继续提供代理。

![节点卡片网格示例（虚拟节点）](docs/screenshots/node-grid.png)

## 构建和安装

使用已安装的 CMake、C++ 编译器、Qt 5.11+（Core、Widgets、Network、Qml、DBus、Test）和
`dde-control-center-dev`。YAML 解析器已随源码提供，不需要安装新依赖。

```sh
./tools/build-deb.sh
sudo dpkg -i dist/dcc-proxy_0.1.0_$(dpkg --print-architecture).deb
```

脚本根据**构建机器**的架构自动选择官方 Mihomo v1.19.12 内核，只打包对应架构。
内核是官方预编译二进制；插件与后台管理程序在本机编译。下载缓存保存在 `cores/`，
构建和打包不自动执行 apt 安装。`--without-core` 可生成使用现有内核的包。
跨架构构建需要对应工具链和 Qt sysroot，也可直接在目标架构机器执行脚本。

控制中心接口优先使用 `/usr/include/dde-control-center`。没有开发头文件时可使用
仓库内接口快照，但最终发布必须使用目标系统的实际接口头，以避免 ABI 不兼容。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
./build/dcc-proxy-preview
```

## 使用

1. 重新打开控制中心，进入“私人代理”。
2. 在“订阅”顶部粘贴 URL，点击“URL 导入”，也可导入 Clash / Mihomo YAML 文件。
3. 在“设置”确认内核路径和控制器地址，保存并连接。
4. 选择订阅，点击“使用此配置”，等待内核启动。
5. 在“节点”选择策略组，选中节点后点击“使用选中节点”或双击节点。
6. 按需开启系统代理或 TUN。TUN 要求内核已具有系统所需的网络管理权限。

节点使用自适应卡片网格显示，每张卡片包含国旗、名称、协议和延迟。
国旗优先识别节点名中的地区旗帜，再识别中文地区名、常见英文地名及 ISO 地区代码。
图标使用内置 SVG，不依赖国旗字体；不能识别的节点使用地球图标，避免猜测位置。
节点名较长时省略显示，悬停可查看完整原始名称和内核信息；切换请求始终使用原始名称。
测速时对应卡片的勾选位置显示旋转进度环；整组测速逐节点回收结果，失败节点不会覆盖其他节点的成功结果。
每个节点可设置 1–30 秒的测速超时。定时刷新只更新变化的数据，保留卡片、选中项和滚动位置。

URL 下载参考 Clash Party 的 User-Agent，支持重定向、订阅流量信息和更新周期。
“订阅设置”可设置 User-Agent、Authorization、超时、更新周期和本地代理下载。
支持完整 YAML，以及原始或 Base64 编码的 Hysteria2、Trojan、VLESS、Shadowsocks、TUIC URI 列表。
其他分享格式建议由订阅服务输出 Clash / Mihomo YAML。

默认控制器地址为 `http://127.0.0.1:19090`，首次运行生成随机 API 密钥。
连接已运行内核时填写其控制器地址和密钥。远端控制器要求 HTTPS。

后台通过当前用户专用 Unix socket 接受控制命令，插件退出不会结束后台。
勾选登录自启后由 systemd 用户服务启动。取消勾选不停止当前内核。

```sh
systemctl --user status dde-mihomo-daemon.service
/usr/lib/dcc-proxy/dde-mihomo-daemon --command status
/usr/lib/dcc-proxy/dde-mihomo-daemon --command stop
```

本地配置在 `~/.config/dcc-proxy/`，目录 0700、文件 0600。备份包含订阅和 API 密钥。
后台日志轮转为两个 2 MiB 文件，界面实时日志最多保留 2000 行。
测试可通过 `DCC_PROXY_DATA_DIR` 指定隔离数据目录，不影响已有订阅。

## 功能范围

详见 [Clash Party 功能对照](docs/feature-matrix.md)。本项目使用原生控件重写，
并非将 Electron 窗口嵌入控制中心。当前版本不能宣称与 Clash Party v2.0.3 全功能等价。
构建与真实内核验证见 [测试记录](docs/TESTING.md)。

## 来源与许可证

本项目沿用控制中心插件接口的 LGPL-3.0-or-later 许可，见 `LICENSE`。
`third_party/dcc-interface` 来自 dde-control-center 的接口快照。
`third_party/yaml-cpp` 来自 yaml-cpp 0.8.0（MIT），保留其许可证。
`third_party/flag-icons` 来自 [flag-icons](https://github.com/lipis/flag-icons)
的 4:3 SVG 资源（提交 `086f7e97d657358203916dbe84f61c2bccaa81eb`，MIT），保留其许可证。
功能参考 [Clash Party v2.0.3](https://github.com/mihomo-party-org/clash-party/tree/v2.0.3)，
API 依据 [Mihomo 官方文档](https://wiki.metacubex.one/api/)，未复制上游应用代码。
随包内核来自 [MetaCubeX/mihomo](https://github.com/MetaCubeX/mihomo/releases/tag/v1.19.12)，
其 GPL 许可证与对应源码归档安装在 `/usr/share/doc/dcc-proxy/`。
