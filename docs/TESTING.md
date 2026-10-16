# 验证记录

验证环境：本机 AMD64、远端 UOS 20 ARM64，Qt 5.11.3。
使用环境已安装的构建工具和 Qt，未安装额外依赖。

## 自动测试

`tools/build-deb.sh` 在两个架构上均编译通过并生成对应 deb；内置 QtTest 的 14 项结果全部通过。

覆盖配置保存/合并/备份、安全校验、原始和 Base64 URI 订阅、真实本地 HTTP 请求、
可见 URL 导入按钮、来源 URL 与订阅元数据、节点切换成功读回和失败保留当前节点。
国旗测试覆盖中文/英文/地区代码/表情优先级、未知地区回退，并渲染全部内置 SVG。
网格测试检查宽窗口、窄窗口同一行多列卡片。
插件生命周期测试模拟控制中心延迟销毁旧页面，确认再次打开页面时不会变成空白。
测速回归测试检查单节点和整组进度动画、成功/失败独立完成、旧历史不覆盖新结果，
并确认轮询和手动刷新不销毁现有卡片模型，防止闪烁与丢失选择。

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
真实 URL 订阅读取到 10 个 Hysteria2 节点；初次测试中有 5 个节点测速成功，
选择实际节点后访问 HTTPS 测试端点返回 204。后续复测发现上游连接超时，
因此不把初次成功等同于订阅节点持续可用；界面保留每个节点的实时测试结果和失败原因。

真实订阅、节点密钥、测试机登录凭据和实际屏幕截图均未加入仓库。
`docs/screenshots/node-grid.png` 使用虚拟节点生成，仅作布局示例。

## 重现

```sh
./tools/build-deb.sh
python3 tests/smoke_real_core.py \
  --service build-$(dpkg --print-architecture)/dde-mihomo-daemon \
  --core cores/$(dpkg --print-architecture)/mihomo \
  --preview build-$(dpkg --print-architecture)/dcc-proxy-preview
```
