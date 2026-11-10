#pragma once
#include <Arduino.h>

#include "../state.h"

// 从 Mac 上的 codex-desk-status sidecar 拉取 Codex 状态与 Grok Bot 用量。
// 只读 GET /status，不依赖 Mac 在线：拉不到时 Codex/Grok 面板显示「未连接 Mac」或「连接中断」。
void codexBegin();
void codexLoop();
void codexDisconnect();
void codexKick();  // 立即拉取一次（切回市电档时用）

// 当前连接状态：从未同步 / 正常 / 超过 CODEX_STALE_MS 没有成功更新。
CodexLink codexLinkNow();

// sidecar 时间：同步时的 unix + 之后经过的秒数。未同步返回 0。
int64_t codexUnixNow();
