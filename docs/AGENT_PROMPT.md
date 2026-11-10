# 交给开发 Agent 的提示词（直接复制）

你要在 Q 的 Mac 上做一个 ESP32-S3 固件项目，把主路由 Clash 面板（zashboard）的“概览”页做成小屏实时仪表盘。

项目目录：`~/Developer/router-dash-esp32`。先完整阅读：
- `docs/REQUIREMENTS.md`（需求、数据接口、验收标准）
- `docs/PLAN.md`（目录结构、里程碑、风险）
- `docs/reference/zashboard-overview.png`（目标页面截图）
- 参考已有项目 `~/Developer/codex-desk-status`（同款 T-Display-S3 的 LovyanGFX 配置、字体、按键、Wi-Fi 配置写法），**只读参考，不要修改它**。

硬性规则：
1. 秘密：Wi-Fi 密码和 Clash 面板密码只写进 `firmware/include/config.h`（已 gitignore）。不要在终端输出、日志、文档、提交、回复里出现任何密码。需要 Wi-Fi 密码时，从 `~/Developer/codex-desk-status/firmware/include/config.h` 读取并复制进新 config.h，不要打印出来。
2. 路由器只读：只用 GET 请求访问 `http://192.168.50.1:9990`。不要切换节点、关连接、改配置，不要 SSH 进路由器改任何东西。
3. 板子：先做 M0（esptool 只读识别）。如果 `/dev/cu.usbmodem21101` 上就是 Codex 状态屏那块 T-Display-S3，**烧录前停下来问 Q**：覆盖，还是做成双页面。
4. 先出界面预览图（M1），Q 确认后再写固件。
5. 不要 push 到任何远程仓库；本地 git 提交可以，但不能提交 config.h。
6. 不要修改或删除 `~/Developer/TokenEconomizer` 和 `~/Developer/codex-desk-status`。

工具链：`~/.codex-desk-pio-venv/bin/pio`（PlatformIO 已装好，espressif32@6.5.0，LovyanGFX 1.2.36，ArduinoJson 7.4.3）。
烧录：`pio run -t upload --upload-port /dev/cu.usbmodem21101`，串口监视 115200。

每完成一个里程碑，汇报：做了什么、怎么验证的（串口输出或屏幕照片）、下一步、需要 Q 决定的事。
