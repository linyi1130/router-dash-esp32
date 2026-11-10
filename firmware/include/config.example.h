#pragma once
// Copy to config.h (gitignored) and fill in. Never commit config.h.

#define WIFI_SSID      "YourWiFi"
#define WIFI_PASSWORD  "CHANGE_ME"

#define CLASH_HOST     "192.168.50.1"
#define CLASH_PORT     9990
#define CLASH_SECRET   "CHANGE_ME"   // 面板密码，见路由器 Clash 管理面板页面

// [已废弃] 华硕后台登录（ROUTER_HOST / ROUTER_USER / ROUTER_PASS）不再使用，可以从 config.h 里删掉。
// 外网速度改为 UPnP IGD 只读读取，设备不再登录路由器后台，不会挤掉你浏览器的登录。

// 当前节点所在的 Selector 组名（原样，含 emoji/中文；代码里会做 URL 编码）。
// 以 GET /group 实际返回为准，通常为 "🚀 节点选择"。
#define PROXY_GROUP    "🚀 节点选择"

// 屏幕方向：1 或 3（画面颠倒时切换）。横屏 400x300。
#define DISPLAY_ROTATION 1

// 屏幕亮度 0-255。
#define BACKLIGHT 150

// 延迟测试周期（毫秒）。
#define LATENCY_INTERVAL_MS 60000

// ---- Codex / Grok Bot 面板（可选；config.h 里不写这些也能编译，用括号里的默认值）----
// 数据来自 Mac 上的 codex-desk-status sidecar（GET /status）。Mac 关机/睡眠时面板显示「未连接 Mac」，
// 不影响路由器流量、网络信息和延迟。
// #define CODEX_ENABLED     1              // 0 = 完全关闭这个面板（默认 1）
// #define CODEX_MDNS_HOST   "codex-status"   // sidecar 广播的主机名，不带 .local（默认）
// #define CODEX_SIDECAR_HOST ""            // mDNS 失败时的固定地址（默认空）。建议在路由器给 Mac 做 DHCP 保留，再填它的 IP
// #define CODEX_SIDECAR_PORT 8787          // sidecar 端口（默认 8787）
// #define CODEX_POLL_MS     5000           // 拉取间隔（默认 5000）；失败后退避到 30s
// #define CODEX_STALE_MS    20000          // 超过这么久没成功更新就显示「连接中断」（默认 20000）
// #define CODEX_TOKEN       "CHANGE_ME"    // 与 sidecar 的 CODEX_STATUS_TOKEN 相同；留空表示不带令牌头（默认空）

// ---- 外网速度：UPnP IGD（可选；config.h 里不写这些也能编译，用括号里的默认值）----
// 通过 SSDP 自动发现路由器的 UPnP 服务（端口会随路由器重启变化，不要写死），读 WAN 口累计字节算速率。
// 需要路由器开启 UPnP（华硕默认开启）。不需要任何账号密码。
// #define UPNP_SAMPLE_MS    2000           // 采样间隔（默认 2000）
// #define UPNP_DESC_URL     ""             // 发现不了时的备用：设备描述地址，如 "http://192.168.50.1:32795/rootDesc.xml"（默认空 = 自动发现）
