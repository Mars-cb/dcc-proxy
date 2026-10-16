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
节点 DNS 回归测试验证兼容选项只补充缺失的节点解析器，保留订阅其余 DNS 配置及明确指定的节点 DNS。

## 真实内核集成测试

两个架构均运行 `tests/smoke_real_core.py`，使用独立临时数据目录和官方 Mihomo v1.19.12：

- DIRECT 选择后，实际 HTTP 流量到达本地目标。
- REJECT 选择后，实际流量被阻断。
- 关闭插件预览窗口后，内核 PID 不变且仍能代理流量。
- 无效配置验证失败时，原内核 PID 和流量保持正常。
- 修改运行模式后 API 读回一致。
- 停止后台后释放内核进程。

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
