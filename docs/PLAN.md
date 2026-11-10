# 项目规划

## 目录
```
router-dash-esp32/
  docs/            需求、规划、参考截图
  firmware/
    platformio.ini
    include/config.example.h   （复制成 config.h 填入 Wi-Fi 和面板密码）
    src/main.cpp
    src/net/clash_client.{h,cpp}   /traffic /memory 流式读取；/connections 定时拉取
    src/net/latency.{h,cpp}        站点延迟测试
    src/ui/dashboard.{h,cpp}       绘制
    src/ui/fonts/                  中文字体
  tools/preview/   （可选）在 Mac 上用 Python 或 HTML 渲染 320×170 预览图，方便先给 Q 看效果
```

## 里程碑
- **M0 确认硬件**（只读）：用 esptool 读 chip/flash/PSRAM；确认是不是 Codex 状态屏那块。如果是，停下来问 Q 怎么处理。
- **M1 预览稿**：先出 320×170 的界面预览图（PNG），给 Q 确认布局后再写固件。
- **M2 数据层**：Wi-Fi 连接 + `/version` 鉴权 + `/traffic` 流式解析，串口打印速度。
- **M3 主屏**：速度大字 + 60 秒折线 + 连接数/内存/累计流量。
- **M4 延迟**：5 个站点延迟测试和颜色规则。
- **M5 第二页与按键**：节点和 Top 主机；亮度和息屏。
- **M6 稳定性**：断线重连、错误状态、24 小时运行测试、README。

## 风险
- `/connections` JSON 很大：用 ArduinoJson 的 Filter 加 PSRAM 分配器，或者手写流式解析。
- 流式接口：HTTPClient 默认读完才返回。要改用 `WiFiClient` 按行读取 chunked 响应，或者每秒轮询一次 `/traffic` 只取第一行再断开（简单但开销稍大）。
- 中文和 emoji 节点名：字库体积要控制；emoji 一律过滤。
- 和 Codex 状态屏共用一块板：烧录会覆盖原固件，必须先问 Q。
