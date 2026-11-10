# 交接文档

> 本文档记录当前项目的实际状态、硬件、架构、配置与待办，供后续接手者（人或 AI）快速恢复上下文。
> 规范文档见同目录 `REQUIREMENTS.md`、`PLAN.md`、`AGENT_PROMPT.md`。

## 1. 项目概述

为华硕路由器 RT-AX88U Pro 上的 Clash（mihomo）做一个 ESP32-S3 小屏状态面板：
- 读取 Clash REST API，显示代理流量、连接数、内存、当前节点、延迟等。
- 读取路由器 WAN 口总流量（UPnP IGD，不登录后台；区分"直连"与"代理"流量）。
- 显示"网络信息"：国内出口 IP 与全球节点出口 IP（含归属地）。
- 单色反射屏，横屏 400×300，两页切换。

## 2. 硬件

| 项目 | 值 |
|------|----|
| 开发板 | **Waveshare ESP32-S3-RLCD-4.2**（注意：不是规范文档里的 LilyGO T-Display-S3） |
| 主控 | ESP32-S3-WROOM-1-N16R8（16MB Flash，8MB OPI PSRAM） |
| 屏幕 | ST7305 反射式单色 LCD，4.2"，无背光 |
| 分辨率 | 原生 300×400，用 `U8G2_R1` 旋转为横屏 400×300 |
| 接口 | SPI（4-wire HW） |

引脚定义见 [board.h](../firmware/src/board.h)：

| 引脚 | 用途 |
|------|------|
| GPIO11 / 12 / 5 / 40 / 41 | SCK / MOSI / DC / CS / RST |
| GPIO0 | BOOT 按键（短按息屏/亮屏） |
| GPIO18 | KEY 按键（短按切页 / 长按息屏） |

## 3. 工具链与烧录

- PlatformIO：`~/.codex-desk-pio-venv/bin/pio`
- 烧录端口：`/dev/cu.usbmodem21101`
- 波特率：115200

```bash
cd firmware
~/.codex-desk-pio-venv/bin/pio run                 # 编译
~/.codex-desk-pio-venv/bin/pio run -t upload -p /dev/cu.usbmodem21101   # 烧录
~/.codex-desk-pio-venv/bin/pio device monitor -p /dev/cu.usbmodem21101  # 看串口日志
```

`platformio.ini` 沿用 `lilygo-t-display-s3` 板型（同为 ESP32-S3 N16R8 + OPI PSRAM，已验证可烧录），
显示用 U8g2 + 自定义 ST7305 驱动（见 [ST7305_U8g2.cpp](../firmware/src/ui/ST7305_U8g2.cpp)）。

依赖：`U8g2@2.36.18`、`ArduinoJson@7.4.3`。

## 4. 目录结构

```
firmware/
  platformio.ini
  include/
    config.h          # 秘密（WiFi/Clash/路由器密码），已 gitignore，需手填
    config.example.h  # 占位模板（CHANGE_ME）
  src/
    board.h           # 引脚 + 逻辑尺寸
    state.h           # 全局状态 DashState g
    main.cpp          # WiFi + 各数据源 begin/loop 编排
    net/
      codex_client.cpp/.h   # Mac 上 codex-desk-status sidecar：Codex 状态 + Grok Bot 用量（GET /status）
      clash_client.cpp/.h   # Clash REST（流量/内存/连接/节点/版本）
      upnp_wan.cpp/.h       # UPnP IGD：SSDP 发现 + SOAP 读 WAN 口累计字节 -> 外网速度（不登录路由器）
      upnp_wan_logic.h      # 上面的纯逻辑（解析/回绕/速率），固件与 host_test 共用
      ipinfo_client.cpp/.h  # 国内/全球出口 IP 与归属地
      latency.cpp/.h        # 5 站点延迟：后台 FreeRTOS 任务，经 Clash 延迟接口测（不再自己做 TLS）
      latency_logic.h       # 延迟接口的路径编码/响应解析（纯逻辑，host_test 覆盖）
      battery.cpp/.h        # 电池电压（GPIO4 ADC1，1/3 分压）-> g.batt*
      battery_logic.h       # 电压->百分比曲线、EMA、±1% 滞回、5 分钟趋势充电推断（纯逻辑，host_test 覆盖）
      power.cpp/.h          # 市电/电池两档刷新频率：powerIv() 取当前周期，powerLoop() 切档
      power_logic.h         # 两档周期表 + 切档状态机（纯逻辑，host_test 覆盖）
    ui/
      ST7305_U8g2.cpp/.h    # 自定义 ST7305 驱动
      dashboard.cpp/.h      # 布局与绘制（两页 + 按键）
```

## 5. 数据源与架构

所有网络请求只读（GET，以及只读的 UPnP SOAP Get* POST）；不再登录华硕后台。

### 5.1 Clash REST API
- 地址：`http://192.168.50.1:9990`，头 `Authorization: Bearer clash`（`CLASH_SECRET`）。
- 使用接口：`/traffic`、`/memory`（长连接流式），`/connections`、`/version`、`/group`、`/proxies/<组名>`。
- 字段落地：`downSpeed/upSpeed`、`memInuse`、`connCount`、`downloadTotal/uploadTotal`、`currentNode`、`topDown/topUp`。

### 5.2 外网速度：UPnP IGD（取代旧的华硕后台登录，见第 11 节）
- SSDP M-SEARCH（组播 + 单播到网关；ST = InternetGatewayDevice:1 / WANCommonInterfaceConfig:1）→ 取 `LOCATION`（只接受主机 = 默认网关的应答）。
- GET 设备描述 XML（实机：`http://192.168.50.1:<动态端口>/rootDesc.xml`，端口会随路由器重启变化，**不写死**）→ 找 `WANCommonInterfaceConfig:1` 的 `controlURL`（实机 `/ctl/CmnIfCfg`）。
- 每 `UPNP_SAMPLE_MS`（2s）发 SOAP `GetTotalBytesReceived` / `GetTotalBytesSent`（无认证，只读，实测 2–5ms），两次累计值算速率（`wanDownSpeed/wanUpSpeed`）。
- 计数器是 **32 位无符号**（到 2^32-1 回绕）：增量 = `(cur - prev) & 0xFFFFFFFF`。首个样本只做基线；增量超过 400MB/s 视为计数器重置（路由器重启）丢弃一个样本；样本间隔 >120s 只当新基线。
- 连续 3 次读取失败重新发现（5s→10s→…→60s 退避）；10 秒没有有效样本则外网速度清零、`wanValid=false`。
### 5.3 网络信息（国内 / 全球出口 IP）
- 依赖 Clash 透明代理分流：
  - `http://myip.ipip.net` 走 DIRECT → 返回**国内**出口 IP + 中文归属地。
  - `http://ip-api.com/json?fields=...` 走 PROXY → 返回**全球节点**出口 IP + 归属地。
- 结果落地：`chinaIp/chinaLoc`、`globalIp/globalLoc`，每 60 秒刷新一次。
- 实测：国内 `203.0.113.10 / 中国 某省 某市 某运营商`，全球 `198.51.100.20 / United States Example Hosting（均为文档示例 IP）`。

## 6. 配置（秘密，需手填）

`firmware/include/config.h`（已 gitignore，不提交）需手动填写：

```c
#define WIFI_SSID "..."        // WiFi
#define WIFI_PASSWORD "..."
#define CLASH_SECRET "clash"   // Clash 面板密码
#define CLASH_HOST "192.168.50.1"
#define CLASH_PORT 9990
```

`ROUTER_HOST` / `ROUTER_USER` / `ROUTER_PASS` **已废弃**：固件不再引用它们，config.h 里留着无害，建议删掉（尤其是管理员密码）。

模板见 `config.example.h`。系统会拒绝 AI 硬编码密码，需用户手填。

## 7. 当前 UI

- 字体：中文 `wqy14`（14px）、速度数值 `helvB14`、IP/延迟数值 `helvR08`（8px）、小号 ASCII `6x13`。
- **首页（page 0）**：
  1. 速度两行：`外网`（路由器 WAN 总流量，UPnP）+ `代理`（Clash 流量）。
  2. 网络信息左右两列：左 `国内`、右 `全球`，各含 IP + 归属地。
  3. 延迟 5 行：Baidu / CF / OpenAI / Github / YouTube（状态方块 + ms 值）。
- **第二页（page 1）**：当前节点 + 下载最多 + 上传最多。
- 按键：KEY 短按切页 / 长按息屏；BOOT 短按息屏。
- 无折线图、无版本号、无页脚（均已按用户反馈移除）。

## 8. 完成状态

已完成并烧录验证：
- ST7305 驱动 + 400×300 横屏显示。
- Clash 数据层（流量/内存/连接/节点/延迟）全部跑通。
- ~~华硕登录 + WAN 总流量读取~~：已被 UPnP IGD 取代（第 11 节）。
- 网络信息国内/全球 IP 解析正确（串口已验证）。
- 首页统计区（连接/内存/下载/上传）已替换为"网络信息"，并改为左右两列。

## 9. 待办 / 注意事项

1. ~~临时调试打印~~：已移除（见第 10 节）。
2. ~~全球归属地较长~~：已改为「国家 + 城市」（见第 10 节）。
3. **最新一轮 UI 调整尚未实物确认**：字体缩小到 8px、删除页脚、网络信息改左右两列这几步已烧录，但未能通过照片核对实际观感（图片审核接口曾拦截），建议上电后按键确认排版。
4. 网络信息每 60 秒刷新一次。（华硕后台登录已移除，见第 11 节。）

## 10. 2026-11-10 改动：v3 布局 + Codex / Grok Bot 面板 + 华硕会话修复

> 状态：**只编译通过（`pio run`），未烧录、未上板验证，未 commit。** LilyGO 的 Codex 状态屏已不存在，其逻辑移植到本板。

### 10.1 首页新布局（预览图在 box 的 /workspace/router-preview/v3/，Mac 上没有；坐标以 `ui/dashboard.cpp` 顶部常量为准）
- 上半：速度两行 + 国内/全球两列（中间竖线 x=200，IP 改为 6x13，全球归属地只留国家+城市）。
- 下半左列：延迟（基线 157 起，行距 18）+ 横线 y=237 + **Grok Bot 用量块**（标题+百分比+进度条+「约 N 小时/天后重置」）。
- 下半右列：**Codex 面板**（状态图标+标签、窗口名、本周/上下文两条进度条、token 数+本轮时长、页脚）。
- Grok 用量 ≥85%：标题行反白 + 粗边框进度条；Codex「额度将尽」整行反白。
- Mac 不在线：**延迟照常显示**；Codex 显示「未连接 Mac」（曾连上则「连接中断」并保留上次数据），Grok 显示虚线框 +「未连接 Mac」+ `--`。
- Grok 应用未运行/读不到：保留最近缓存的百分比，底行「应用未运行 / 应用未连接」。
- 第 2 页（节点 + Top 主机）未改。不显示 Codex 的 `summary`。
- wqy14 字库没有「·」等标点，`state.h: normalizeGlyphs()` 统一换成空格；emoji 由 `sanitizeUtf8` 去掉。

### 10.2 数据来源：codex-desk-status sidecar（Mac，`~/Developer/codex-desk-status/host/`）
- `net/codex_client.cpp`：先 mDNS（`codex-status.local`），失败用固定地址；`GET /status` 每 `CODEX_POLL_MS`（5s）一次，连接/读取超时各 800ms，失败退避 5→10→20→30s，超过 `CODEX_STALE_MS` 没成功即「连接中断」。ArduinoJson filter 只取需要的字段。带 `X-Status-Token` 头（若配置了 `CODEX_TOKEN`）。401 时面板显示「令牌错误」。
- sidecar 新增 `grok` 段（`host/grok_status.py`，只读 `desktop-status.json` 和 `*weekly-usage.cache`，绝不碰 roster/对话/密钥）。
- **config.h 新键（都可省略，代码里有默认值；说明见 `config.example.h`）**：`CODEX_ENABLED`、`CODEX_MDNS_HOST`、`CODEX_SIDECAR_HOST`、`CODEX_SIDECAR_PORT`、`CODEX_POLL_MS`、`CODEX_STALE_MS`、`CODEX_TOKEN`，以及 UPNP 相关见第 11 节（`ASUS_SAMPLE_MS` 已随华硕登录移除）。
- Mac 要做的事：启动 sidecar（`python3 host/codex_status.py`，或装 launchd）；设令牌环境变量 `CODEX_STATUS_TOKEN` 并在 config.h 写同样的 `CODEX_TOKEN`；在路由器给 Mac 做 DHCP 保留（可选，填 `CODEX_SIDECAR_HOST`）。

### 10.3 华硕会话修复（已作废）
- 这一轮曾给 `asus_client.cpp` 加了登出/退避，但根因（设备占着后台会话）无法根治，已在第 11 节整个移除，改用 UPnP。

### 10.4 待办
1. 烧录前确认串口上插的是 Waveshare 板（`/dev/cu.usbmodem21101`）。
2. 上板核对：虚线框与文字间距、反白行对比度、各状态（尤其 8bit 字体边缘）。
3. ~~验证 Logout.asp~~：不再需要（已不登录后台）。
4. sidecar 首次启动在大会话文件下很慢（`~/.codex/sessions` 约 2GB，python3.9 下首次 publish 超过 1 分钟），期间 `/status` 会超时，面板显示「连接中断」。

## 11. 2026-11-10 改动：外网速度改用 UPnP IGD，彻底不登录华硕后台

> 状态：**只编译通过 + 主机端测试通过，未烧录、未上板验证，未 commit。**

### 11.1 为什么
设备登录华硕后台会占着 httpd 唯一的会话，导致用户的浏览器登不进去。路由器（RT-AX88U Pro，`AsusWRT/4.19.183`，MiniUPnPd 2.3.8，3006.102 系列固件）的 UPnP IGD 无需认证即可只读 WAN 口累计字节，所以改用它。SNMP 不可用（161/UDP 无响应），Clash `/traffic` 只含代理流量。

### 11.2 改了什么
- 新增 `net/upnp_wan.{h,cpp}`（发现 + 轮询）和 `net/upnp_wan_logic.h`（纯逻辑）。写入的字段和以前一样（`g.wanDownSpeed/wanUpSpeed/wanValid`），UI 不变。
- **删除** `net/asus_client.{cpp,h}`（不用 `#if ASUS_LOGIN_ENABLED` 保留，避免残留登录路径和对 ROUTER_* 的依赖）。原版可用 `git show HEAD:firmware/src/net/asus_client.cpp` 找回。
- `main.cpp`：`asusBegin/Loop/Disconnect` 换成 `upnpBegin/Loop/Disconnect`。
- 固件里**没有其他地方**用过华硕登录（WAN IP 等字段本来就不用它：国内/全球 IP 来自 `ipinfo_client`），所以无需替代；若以后要显示 WAN IP，可用 WANIPConnection 的 `GetExternalIPAddress`（`/ctl/IPConn`，同样无认证）。
- config.h 新键（都可省略）：`UPNP_SAMPLE_MS`（默认 2000）、`UPNP_DESC_URL`（默认空 = 自动发现；备用，如 `"http://192.168.50.1:32795/rootDesc.xml"`，端口可能随路由器重启变化）。

### 11.3 如何验证
- 主机端（Mac，只读，不登录）：
  - 单元测试：`cd firmware/host_test && g++ -std=c++17 -Wall -Wextra -I../src/net test_upnp_logic.cpp -o /tmp/t && /tmp/t`（回绕、重置、URL/XML/SOAP 解析、真实脱敏描述 `rootDesc.sample.xml`）。
  - 实机：`g++ -std=c++17 -Wall -Wextra -I../src/net upnp_live.cpp -o /tmp/l && /tmp/l 192.168.50.1 8`（SSDP 发现 + 读计数器 + 打印速率）。
- 上板后：串口应出现 `[upnp] 发现 IGD：192.168.50.1:<端口>/ctl/CmnIfCfg`；屏幕「外网」行有数值。做一次测速（或下载大文件），对比「外网」数值与路由器后台流量页 / Mac 测速结果，应大致吻合。
- 同时浏览器登录路由器后台，应不再被拒绝。

### 11.4 注意 / 风险
- **硬件加速**：WAN 口计数来自内核/UPnP 守护进程，若路由器开了硬件加速（Runner/Flow Cache），被加速的流量可能不计入或少计。尚未验证准确度，务必做上面的测速对比；若明显偏低，备选是 SSH 读 `/proc/net/dev`（要开 SSH + 密钥，需用户决定）。
- UPnP 计数器是 32 位：2 MB/s 约 35 分钟回绕一次，已按无符号回绕处理；采样 2s 不会漏圈。
- 路由器 UPnP 开关被关闭（或固件把 IGD 端口随重启改变）时：端口变化会自动重新发现；UPnP 被关则外网行显示 0，串口反复出现「发现失败」。
- 仅接受来自默认网关的 SSDP 应答，避免局域网里别的设备冒充；若设备不在路由器的直连网段（例如经过另一层路由），需要用 `UPNP_DESC_URL` 指定。
- 路由器管理员密码已不再需要：可以从 `config.h` 删掉 `ROUTER_HOST/USER/PASS` 三行。

## 12. 2026-11-10 上板后修复：外网恒为 0 + 延迟全超时

> 状态：**只编译通过 + 主机端测试通过，未烧录，未 commit。**

- **外网恒为 0 的根因**：`upnpLoop()` 末尾的过期判断 `now - lastGoodMs > kStaleMs` 用的是函数开头的 `now`，而 `poll()` 里 `lastGoodMs = millis()` 更晚几毫秒，无符号相减回绕成巨大正数，刚算出速度就被 `markInvalid()` 清零。修复：逻辑挪到 `upnp_wan_logic.h: isStale()`（有符号比较），用最新 `millis()`；`test_upnp_logic.cpp` 加了回归测试（含「lastGood 比 now 晚」的原样复现）。
- **延迟全超时的根因**：旧实现是 ESP32 自己连站点做 TLS 并等首字节，2 秒预算内，经美国节点的 HTTPS 站点必超时；5 个站点顺序阻塞主循环最多约 10 秒。新实现：`GET /proxies/<代理>/delay?url=<站点>&timeout=5000`（Clash 测，与 zashboard 一致），在独立 FreeRTOS 任务（core 0，栈 10KB）里逐个测，每测完一个就写入 `g.latency[i]`，主循环不再被阻塞。Baidu 走 `DIRECT`（国内规则直连），其余 4 个走 `PROXY_GROUP`（节点选择组，即当前所选节点）。-1 = 超时/出错，-2 = 未测，显示语义不变。周期仍是 `LATENCY_INTERVAL_MS`（60s）。
- 实机 Clash 延迟接口实测（Mac，节点选择组）：CF 1001ms、OpenAI 366ms、Github 418ms、YouTube 1121ms；Baidu 走组会超时（美国节点访问 Baidu 慢），走 DIRECT 22ms。单次请求墙钟最长约 3 秒，所以后台任务读超时设 7 秒。
- 新增串口日志：`[upnp] WAN 下行 .. 上行 .. B/s 有效=.. 状态=.. rx=.. tx=..`，每 10 秒一行。
- 测试：`cd firmware/host_test && for t in test_upnp_logic test_latency_logic; do g++ -std=c++17 -Wall -Wextra -I../src/net $t.cpp -o /tmp/$t && /tmp/$t; done`。

## 13. 电池图标（首页右上角）

> 状态：**只编译通过 + 主机端测试通过，未烧录，未 commit。**

- 硬件：18650 电量经 200k/100k 分压接 **GPIO4（ADC1_CH3）**，12dB 档（`ADC_11db`），`analogReadMilliVolts(4)*3`。**充电芯片 STAT 只接了板上 LED，没有 GPIO**，所以「充电中」是推断：滤波后电压 ≥4.15V，或相邻两次更新上升 ≥6mV（保持 3 次更新≈90s）。没装电池只接 USB 时会读到约 4.2V，显示 100% + 闪电。
- 采样：16 次平均 → EMA（α=0.3）→ 每 30 秒更新一次（开机立即读一次）→ ±1% 滞回；串口每 30 秒一行 `[batt] 原始 .. mV 滤波 .. mV ..% 充电推断=.. 有效=..`。
- 读数 <2.0V 或 >4.5V 视为没接电池，图标隐藏。
- 曲线：18650 静置电压分段线性（4.20V=100% … 3.71V=15% … 3.30V=0%），见 `battery_logic.h`；这是整理的常见曲线，不是官方数据，负载下会偏低。
- UI（`ui/dashboard.cpp: paintBattery()`）：机身 (358,13) 26×13，凸起 (384,17) 2×5，内部按百分比填充；百分比 6x13 右对齐到 x=354、基线 y=24；≤15% 百分比反白；充电时闪电画在百分比左侧。预览图在 box 的 /workspace/router-preview/v4/。
- 上板核对：先看串口 `[batt]` 的 mV 是否合理（满电约 4.1–4.2V，USB 供电充电时偏高），再对比万用表实测，必要时调 `kDividerRatio` 或加校准系数。

## 14. 充电趋势推断 + 市电/电池两档刷新频率

> 状态：**只编译通过 + 主机端测试通过，未烧录，未 commit。**（取代第 13 节里「相邻两次上升 ≥6mV」的旧充电规则。）

### 14.1 充电推断（`battery_logic.h: ChargeDetector`）
- 环形缓冲存滤波后的 mV + 时间戳，**窗口按时间（≈5 分钟）算，与采样周期无关**（市电 30s、电池 60s 都行）。
- 滤波电压 ≥4.15V（平台期，降到 4.13V 以下才退出）→ 充电（含没装电池只接 USB 的 ~4.2V、充满后仍插着）。
- 否则按「折算到 5 分钟的净变化」判断：>+10mV 充电；≤+3mV 未充电；中间保持原状态（滞回）。数据不足 3 分钟不下结论，**开机为 Unknown，按市电档处理**。
- 参考日志：充电中 3795→3802mV/2 分钟 ≈ +17mV/5 分钟，能判出充电。

### 14.2 两档频率（`power_logic.h`；`power.cpp` 用 config.h 的 `UPNP_SAMPLE_MS/CODEX_POLL_MS/LATENCY_INTERVAL_MS` 覆盖市电档）
| 项 | 市电 | 电池 |
|---|---|---|
| UPnP 外网采样 / 清零阈值 | 2s / 10s | 10s / 25s |
| 延迟一轮 | 60s | 5 分钟 |
| Codex+Grok 拉取 / 退避上限 | 5s / 30s | 30s / 120s |
| 「连接中断」阈值 | CODEX_STALE_MS | ×6 |
| Clash /connections | 5s | 30s |
| ipinfo 刷新 | 60s | 10 分钟 |
| 电池采样 | 30s | 60s |
| 屏幕定时重绘 | 1s | 5s（换页/链路变化仍立即重绘） |
- Clash 的 /traffic、/memory 是长连接流，两档都保持（电池档下这条连接仍会让 Wi-Fi 一直收包，是最大的剩余耗电点；以后可考虑改成按需轮询）。
- 切档：目标档位稳定 **60 秒**才切（`ProfileSelector`）。切回市电时立即 `upnpKick/codexKick/clashKick/ipinfoKick/latencyKick/battKick` 触发一次刷新。读不到合理电压（没电池）→ 市电档。
- 串口：`[power] 充电状态：未知 -> 充电`、`[power] 切换到电池/市电档：…`；`[batt]` 行增加「充电=…」和「5分钟趋势 +N mV」。

### 14.3 注意
- **刚插上充电线最多要几分钟才识别为充电**（要 ≥3 分钟趋势），期间仍是电池档低频；若电压 ≥4.15V 则立即判充电。
- 满电后拔掉线：电压仍 ≥4.15V 时按市电档，降到 4.13V 以下并出现下降趋势后才进电池档，之后切档还要 60 秒。
- 没装电池只接 USB 时读数约 4.2V → 显示 100% + 闪电、市电档（符合预期）。

## 15. 减少屏幕闪烁：帧差分提交（未烧录验证）

- 原因推断：以前每次重绘（市电每秒）都 `sendBuffer()` 整屏 15200 字节；固件和驱动都没有周期性 init/清屏/开关屏/反显（`fullInit` 只在 `lcd.begin` 开机一次；驱动不处理 SET_POWER_SAVE，息屏键实际不动面板）。
- 驱动的 DRAW_TILE 回调支持任意 `x_pos/cnt`（按 12 像素列组发送），所以 `updateDisplayArea` 可用。
- `src/ui/frame_diff.h`（纯逻辑，`host_test/test_frame_diff.cpp`）比较上一帧拷贝与当前缓冲：相同则跳过；不同只发变化图块段（同行相距 ≤2 图块合并）；首帧/重新点亮/段数 >160 整屏。
- 串口每 60 秒 `[ui] 近60秒：画了N帧（局部/整屏） 跳过N帧 发送N字节`。
