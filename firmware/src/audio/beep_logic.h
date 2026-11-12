#pragma once
// Codex 完成提示音的纯逻辑（不依赖 Arduino），固件和主机端测试共用。
#include <stdint.h>
#include <string.h>

// 只在主窗口状态从 running 变为 complete 时响一声。
// 第一次同步、重连后的首帧（prevValid=false）、中断、切到已经是 complete 的窗口，都不响。
// 看的是 sidecar 的 status 字段，不是 pet_state。
inline bool shouldBeepOnCodexStatus(bool prevValid, const char* prev, const char* curr) {
  if (!prevValid || prev == nullptr || curr == nullptr) return false;
  return strcmp(prev, "running") == 0 && strcmp(curr, "complete") == 0;
}

// 任务正在跑时把轮询加快到 1 秒（电池档默认 30 秒会把提示音拖得很晚）。
// 若默认间隔已经更短，保持更短的那个。
inline uint32_t codexPollMsForStatus(const char* status, uint32_t defaultMs) {
  if (status != nullptr && strcmp(status, "running") == 0) {
    constexpr uint32_t kFastMs = 1000;
    return defaultMs < kFastMs ? defaultMs : kFastMs;
  }
  return defaultMs;
}
