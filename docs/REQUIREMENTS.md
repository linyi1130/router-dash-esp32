# 路由器 Clash 面板 → ESP32-S3 小屏：需求文档

## 1. 背景
主路由 RT-AX88U Pro（192.168.50.1，koolshare 固件）上运行 MerlinClash，内核是 mihomo（Clash.Meta）v1.19.3。
Web 面板是 zashboard，地址是 `http://192.168.50.1:9990/ui/zashboard/#/overview`（截图见 `docs/reference/zashboard-overview.png`）。
目标：把这个“概览”页的核心信息做成固件，常亮显示在 Mac 上连着的 ESP32-S3 小屏上。数据直接从 Clash 的 REST API 读取，不经过 Mac。

## 2. 硬件
- 当前连接的板子：Espressif 原生 USB（USB JTAG/serial），MAC `AA:BB:CC:DD:EE:FF`（已打码），Mac 上的端口是 `/dev/cu.usbmodem21101`。
- 这个端口和 `~/Developer/codex-desk-status` 用的 LilyGO T-Display-S3（ST7789 170×320，8 位并口，16MB Flash + OPI PSRAM）是同一个。
  **开工前必须先确认型号**：`~/.codex-desk-pio-venv/bin/esptool.py --port /dev/cu.usbmodem21101 flash_id`，看 Flash 和 PSRAM 大小，再拍照或问 Q 确认屏幕型号。
- 如果确认就是正在跑 Codex 状态屏的那块，**烧录前必须先问 Q**：是覆盖掉，还是把路由器面板做成同一固件里的第二页（按键切换）。
- 网络：2.4GHz Wi-Fi `YourWiFi`（示例名，真实名称只放在 config.h）。密码只放在 gitignore 的 `firmware/include/config.h` 里（可以参考 codex-desk-status 的 config.h 结构）。**禁止**把密码写进聊天、日志、文档或提交。

## 3. 数据源（已实测可用）
基址 `http://192.168.50.1:9990`，请求头 `Authorization: Bearer <面板密码>`。面板密码在路由器 Clash 页面上能看到，只写进 config.h。

| 屏幕字段 | 接口 | 说明 |
|---|---|---|
| 上传/下载速度 | `GET /traffic` | 流式（chunked），每秒推一行 `{"up":B/s,"down":B/s}` |
| 内存使用 | `GET /memory` | 流式，每秒一行 `{"inuse":字节,"oslimit":0}`；第一帧可能是 0，要忽略 |
| 连接数、累计下载/上传 | `GET /connections` | 返回 `downloadTotal`、`uploadTotal`、`memory`、`connections[]`。连接数 = 数组长度。JSON 很大（70+ 条），要用 ArduinoJson 过滤器只解析需要的字段或流式解析，避免爆内存 |
| 最多下载/上传主机、源 IP、节点 | `GET /connections` | 按 `metadata.host`（为空时用 `sniffHost` 或 `destinationIP`）、`metadata.sourceIP`、`chains[0]` 聚合 `download`/`upload` 后取最大值 |
| 内核版本 | `GET /version` | `{"meta":true,"version":"v1.19.3"}` |
| 当前节点 | `GET /proxies/🚀 节点选择`（URL 编码） | `now` 字段；组名以实际为准，`GET /group` 可以列出 |
| 延迟（Baidu/Cloudflare/OpenAI CDN/Github/YouTube） | ESP32 自己测 HTTP 延迟 | 和 zashboard 一样：对每个站点发 HEAD/GET 请求，统计到首字节的毫秒数，每 60 秒测一次。ESP32 的流量会经过 Clash 透明代理，所以结果和网页上一致 |
| 出口 IP（国内/全球） | 可选，v2 再做 | 网页上本来就打码，小屏默认不显示 |

## 4. 显示内容（横屏 320×170，深色主题，配色参考截图）
- 第 1 行（大字号）：↓ 下载速度、↑ 上传速度（自动换算 B/s、kB/s、MB/s）。
- 第 2 区：最近 60 秒上下行速度折线图（两条线，两种颜色，和网页一致：上传青色、下载紫色）。
- 第 3 行（小字号）：连接数、内存、累计 ↓/↑（GB）。
- 第 4 行（小字号）：5 个站点的延迟。颜色和网页一致：<300ms 绿，300–800ms 黄，>800ms 红，超时显示“×”。
- 可选第二页（按键切换）：当前节点名、最多下载主机/源 IP、最多上传主机/源 IP。
- 状态提示：Wi-Fi 断开、API 401（密码错）、API 超时，要分别显示清楚的中文状态。不要卡死，也不要白屏。
- 中文字体：节点名里有中文和 emoji。中文用 codex-desk-status 已有的字体方案；emoji 去掉或替换。

## 5. 刷新与功耗
- 速度和内存：保持 `/traffic`、`/memory` 长连接，每秒刷新数字和图表。断线后指数退避重连（1s→30s）。
- `/connections`：每 5 秒拉一次（数据量大）。
- 延迟：每 60 秒测一次。版本和节点：每 5 分钟一次。
- 这是插电常亮的设备，不追求省电，但屏幕亮度要可调；按键可以息屏或亮屏（参考 codex-desk-status 的按键逻辑）。

## 6. 非功能要求
- PlatformIO + Arduino + LovyanGFX，和 codex-desk-status 用同一套工具链（`~/.codex-desk-pio-venv/bin/pio`）。
- 只读访问路由器 API。**不允许**调用任何会修改 Clash 配置、切换节点、关闭连接的接口（PUT/PATCH/DELETE）。
- 不 SSH 到路由器修改任何东西。
- 长时间运行（≥24 小时）不重启、不内存泄漏：串口每 10 分钟打印一次可用堆内存。
- 秘密只放在 `firmware/include/config.h`（已 gitignore），仓库里只提交 `config.example.h`。

## 7. 验收标准
1. 烧录后 10 秒内连上 Wi-Fi 并显示实时速度，数字和 zashboard 网页同时对比，误差在 1 秒刷新范围内。
2. 连接数、内存、累计流量和网页一致（允许 5 秒延迟）。
3. 5 个站点延迟能显示，颜色规则正确。
4. 把 config.h 里的密码改错，屏幕显示“密码错误(401)”；拔掉路由器 WAN 或停 Clash，屏幕显示“API 不可用”，恢复后自动恢复。
5. 连续运行 24 小时，可用堆内存没有持续下降。
6. 交付：源码、README（怎么配置和烧录）、屏幕实拍或模拟预览图。
