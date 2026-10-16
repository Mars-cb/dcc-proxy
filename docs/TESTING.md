# 验证记录

验证环境：本机 AMD64、远端 UOS 20 ARM64，Qt 5.11.3。
使用环境已安装的构建工具和 Qt，未安装额外依赖。

## 自动测试

`dpkg-buildpackage -us -uc -b` 和 `tools/build-deb.sh` 支持标准 Debian 与 CPack 两种打包方式。
标准打包已在两个架构上完整编译、执行测试并生成对应 deb；内置 QtTest 的 18 项结果全部通过。

覆盖配置保存/合并/备份、安全校验、原始和 Base64 URI 订阅、真实本地 HTTP 请求、
可见 URL 导入按钮、来源 URL 与订阅元数据、节点切换成功读回和失败保留当前节点。
国旗测试覆盖中文/英文/地区代码/表情优先级、未知地区回退，并渲染全部内置 SVG。
网格测试检查宽窗口、窄窗口同一行多列卡片。
插件生命周期测试模拟控制中心延迟销毁旧页面，确认再次打开页面时不会变成空白。
测速回归测试检查单节点和整组进度动画、成功/失败独立完成、旧历史不覆盖新结果，
并确认轮询和手动刷新不销毁现有卡片模型，防止闪烁与丢失选择。
卡片点击测试验证主体区域直接切换，点击未选中节点的延迟或失败状态仅测速，
既不改变当前选中项，也不发送切换节点请求。

绕过测试覆盖规范化和去重、无效输入拒绝、设置恢复与空列表语义，
并通过现有 Qt JavaScript 引擎执行实际 PAC 函数，验证域名、IPv4/IPv6 CIDR 和 DNS 解析后的路由。
测速回归还验证 Clash Party 默认 5000 ms / 50 并发、策略组地址继承、用户地址覆盖与持久化、
Provider 健康检查请求，以及统一延迟默认开启、用户明确关闭时保留。
节点 DNS 回归测试验证兼容选项只补充缺失的节点解析器，保留订阅其余 DNS 配置及明确指定的节点 DNS。

## 真实内核集成测试

两个架构均运行 `tests/smoke_real_core.py`，使用独立临时数据目录和官方 Mihomo v1.19.12：

- 没有已启用配置记录时，“启动”会使用首个导入订阅。
- DIRECT 选择后，实际 HTTP 流量到达本地目标。
- REJECT 选择后，实际流量被阻断。
- 关闭插件预览窗口后，内核 PID 不变且仍能代理流量。
- 无效配置验证失败时，原内核 PID 和流量保持正常。
- 候选配置端口被其他程序占用时明确报错，现有内核 PID 和代理流量保持正常。
- 修改运行模式后 API 读回一致。
- 停止后台后释放内核进程。

## 本机与现有 Clash 共存验证

0.1.1 已安装到本机。真实订阅测试使用独立临时数据目录、随机控制器/代理端口、
随机控制器密钥，并明确关闭测试实例的系统代理联动；所有请求显式指向测试实例。
测试前后检查现有 Mihomo PID 和桌面代理模式/地址/绕过列表，确认未被替换或改变。

同一个日本节点、同一个 `https://www.gstatic.com/generate_204` 地址和 5000 ms 超时，
各预热一次、采样三次：统一延迟关闭为 326/274/327 ms（中位数 326 ms），
开启为 120/125/120 ms（中位数 120 ms）。数值直接来自官方内核，无显示修正。
隔离代理实际访问 Google、YouTube 均返回 200。此记录只代表该次测试，实际延迟随线路变化。

实现对照 Clash Party v2.0.3 的 `src/main/utils/template.ts`、
`src/main/core/mihomoApi.ts` 和 `src/renderer/src/pages/proxies.tsx`：
默认统一延迟、5000 ms 超时、50 并发、测速 URL 优先级、逐节点回收及 Provider 健康检查。

## 远端实际环境

ARM64 deb 已安装到 `/usr/lib/dde-control-center/modules/libdcc-proxy.so`。
控制中心可加载 `dcc-proxy`，名称为“私人代理”，节点页使用多列国旗卡片。
真实 URL 订阅读取到 10 个 Hysteria2 节点。针对“全部测速失败”，同一 ARM64 内核和同一组节点进行隔离对比：
完整订阅配置下测试失败；仅移除订阅 DNS，或仅增加 `proxy-server-nameserver: [system]` 后测试成功。
最终修复使用后一种方式，保留订阅原有 DNS 规则，只解决节点服务器域名的启动解析问题。
安装修复后的后台，10 个节点中 4 个测速成功；日本约 92–139 ms，美国约 190–215 ms。
选择可用节点后实际 HTTPS 请求返回 204。其余节点仍可能因线路、IPv6 或服务端状态失败，不能保证订阅所有节点可用。

`tests/smoke_system_proxy.py` 在远端实际 DDE Network DBus 上验证：关闭状态保存不启用代理、
手动代理绕过写入/立即修改、内置 PAC 动态更新、空列表、无效输入不改变设置，
以及退出时完整恢复原代理模式、地址、PAC URL 和绕过列表。
新增启动联动验证：内核就绪后自动启用系统代理、无效配置保持原代理、重启跟随新端口、
停止恢复原设置，以及明确关闭自动启用选项时不修改桌面代理。

真实订阅、节点密钥、测试机登录凭据和实际屏幕截图均未加入仓库。
`docs/screenshots/node-grid.png` 使用虚拟节点生成，仅作布局示例。

## 重现

标准 Debian 打包，产物在项目目录上一级：

```sh
dpkg-buildpackage -us -uc -b
```

默认包包含当前架构官方内核及对应源码归档。`cores/` 缓存存在且校验通过时无需下载；
不存在时从官方地址下载。可使用 `DCC_PROXY_BUNDLE_CORE=0` 打包不含内核的版本。
安装不会自动启用登录自启，继续由插件设置控制。

CPack 和内核集成测试：

```sh
./tools/build-deb.sh
python3 tests/smoke_real_core.py \
  --service build-$(dpkg --print-architecture)/dde-mihomo-daemon \
  --core cores/$(dpkg --print-architecture)/mihomo \
  --preview build-$(dpkg --print-architecture)/dcc-proxy-preview
```

系统代理集成测试会短暂修改当前桌面代理，并在结束时恢复，须明确传入测试开关：

```sh
python3 tests/smoke_system_proxy.py \
  --service build-$(dpkg --print-architecture)/dde-mihomo-daemon \
  --allow-system-proxy
```


## 0.1.2 概览与 TUN 回归

Qt 回归共 20 项。新增模拟异步返回顺序的概览测试：内核请求先返回、统计请求延迟返回，
重复刷新时两个标签的文字、位置保持稳定，隐藏连接表不重建。原生 DSwitchButton 验证
停止、启动、操作禁用、失败读回；绕过列表的错误项保留并精确提示。

TUN 测试在全新 user/network namespace 中运行，要求宿主网络标识不同且初始只有 lo。
它创建临时出口网卡、本地 DNS 与 HTTP 服务，临时数据目录、独立端口、关闭系统代理联动，
不使用真实订阅或宿主 Clash。测试验证 Mixed TUN、自动路由、any:53 DNS 劫持、Fake IP
实际 HTTP、MTU 生效、保存时保留 enable、网卡冲突拒绝、热加载与唤醒重载、关闭后恢复所有
路由/规则并保留 DNS。使用现有 setpriv 在测试子进程中撤销 CAP_NET_ADMIN，验证真实
EPERM 创建错误及恢复可用内核；不改变宿主内核文件权限。

```sh
unshare --user --map-root-user --net python3 tests/smoke_tun.py \
  --host-netns "$(readlink /proc/self/ns/net)" \
  --service build/dde-mihomo-daemon --core cores/amd64/mihomo
```

本轮只在本机安装和测试，不同步远端机器；未安装新的依赖。


## 0.1.3 首次授权生效

本机复现：内核文件已为 root:root / 4755，但授权前启动的内核仍以普通用户运行，
CAP_NET_ADMIN 不存在。热加载不能改变已有进程权限。后台现在读取该进程的
`/proc/<pid>/status` 能力位；首次启用时检测到缺失会重新 exec 本插件的内核，
已有权限时继续热加载，不触碰其它 Clash 实例。

隔离 TUN 测试新增：先让内核以撤销 CAP_NET_ADMIN 的状态启动，再模拟完成授权。
启用 TUN 必须自动更换该内核 PID、获得真实能力位，并成功执行 DNS 劫持与 HTTP。
后续保存设置保持 PID。持续缺权的失败场景仍回退到可用配置。


## 0.1.4 离线首次启动与完整数据

远端 10.20.8.241 的启动失败来自缺少 MMDB，官方 GitHub 下载直连 5 秒连接超时，
内核初始化下载等待 90 秒后失败。补齐 MMDB 和订阅缺失的 mixed-port 后，控制器 19090、
混合代理 17890 就绪，显式通过本插件访问百度返回 200、Google generate_204 返回 204。
现有 Clash 进程保持不变。

包内提供 Country.mmdb、GeoIP.dat、GeoSite.dat、ASN.mmdb；固定官方仓库提交、
Git blob 与 SHA-256 校验，复用现有 Python 标准库作为构建工具。安装脚本不执行 apt，
运行时不下载上述基础资源。目标机器插件、后台全部动态库均可解析，Mihomo 是静态程序。

`smoke_geodata.py` 在只有 lo 的独立网络空间中，用全新配置、空数据目录启动真正内核，
配置同时使用 GEOIP、GEOSITE、IP-ASN，并将下载 URL 指向拒绝请求的本地服务。
检查零下载请求、真实代理 HTTP 成功；覆盖 MMDB 与 GeoIP.dat 两种模式，四类下载中断
缓存的备份恢复，以及已有可用旧数据库/geoip.metadb 别名不被覆盖。测试不使用真实订阅。

```sh
unshare --user --map-root-user --net python3 tests/smoke_geodata.py \
  --host-netns "$(readlink /proc/self/ns/net)" \
  --service build/dde-mihomo-daemon --resources cores/geodata
```

GeoIP 等构建缓存完整时，可以用 `tools/prepare-geodata.py --offline` 只校验不下载。
Qt 回归增加缺失 mixed-port 时的默认监听及显式覆写优先级检查。


导入预览回归：URL 导入完成后直接在节点页显示订阅内容；内核地址不可达时也可切换
多个订阅，展示名称/协议/国旗与分组，重复刷新保留卡片。预览不创建后台 socket、不写
active、不发切换/测速请求，不将未启动状态标为节点失败。inline provider、include-all、
正则分组与私密节点字段不进入预览模型也有覆盖。

## JavaScript 覆写兼容回归（0.1.5）

`javascriptOverrideDnsAndProfileName` 通过实际后台 `--eval` 子进程执行用户的 DNS
prepend 脚本：保留原 DNS 顺序、插入 `10.20.0.10`、重复执行不重复插入，覆盖缺失
DNS 和非数组 nameserver，并验证中文订阅名传入第二参数以及 includes 的 NaN、负索引、
Infinity 和类数组行为。`javascriptOverrideErrors` 覆盖运行时异常、字符串异常、语法错误、
无返回值、null、数组、循环引用、异步结果及非法 JSON 输入，验证错误不再被返回值校验掩盖。

## 0.1.5 全功能回归清单

`QA-0.1.5.md` 逐项列出本轮验证与实际覆盖边界。完整 Qt 回归包含真实内核日志、HTTPS
WebDAV、原生密码小眼睛、概览五种有效示例、YAML 覆写说明和 460px 长提示布局检查。

`smoke_real_core.py` 增加混合/HTTP/SOCKS 真实流量、TCP/UDP 冲突、控制器冲突/重复/范围
校验、保存失败原配置保留、离线保存与端口持久化、订阅强制更新及异常退出自动恢复。
应用 HTTP 头保持原样，订阅下载使用中性 UA。

系统代理测试可以使用环境已有 DBus/GLib，在独立会话总线上验证，避免修改宿主设置：

```sh
python3 tests/smoke_private_proxy.py --service build/dde-mihomo-daemon --core cores/amd64/mihomo
```

测试中的模拟 DDE 服务只存在于私有总线；GSettings 使用内存后端。覆盖手动/PAC 端口同步、
显式关闭保持关闭、冲突不影响代理、绕过和完整恢复。无须为自测切换宿主 Clash。


## 0.1.6 默认端口

默认控制器改为 19999，默认混合代理改为 16666；更新默认配置、导入、新建、界面提示及
端口回归预期。0.1.5 及更早章节中的端口是对应版本的历史验证记录。已有显式配置不会
仅因升级被改写；本机按用户要求迁移配置并重启后台/内核使两项生效。


## 三种代理模式实际流量补测

`tests/smoke_modes.py` 使用已安装的后台与官方内核、独立临时目录、随机端口、本地 HTTP
目标及真实 CONNECT 中转。系统代理联动关闭，测试实例不连接宿主会话/系统总线。

- 规则：三个不同目的端口分别命中策略组、DIRECT、REJECT；只允许代理分支访问中转，
  改变策略组选择只影响该分支。
- 全局：将 GLOBAL 设为测试代理后，连 DIRECT/REJECT 规则的目标也实际经过中转；将
  GLOBAL 改为 DIRECT 或 REJECT 后分别直连或阻断，证明全局以 GLOBAL 的当前选择为准。
- 直连：同时将 GLOBAL 和策略组选为 REJECT，所有目标仍直达本地服务，中转连接数不增加。
- 切回规则后恢复原规则行为。每次请求新建连接，避免复用旧连接掩盖模式切换结果。

Qt 回归补充三项中文选项到 rule/global/direct 的对应关系、实际 PATCH 请求与读回、
保存到 controlled.yaml 后合成运行配置，以及重开页面后的读回。

```sh
python3 tests/smoke_modes.py
```

这轮仅增加测试与记录，未更改已安装 0.1.6 的程序逻辑或本机当前模式。
