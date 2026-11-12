#pragma once

// Pure page / pet helpers shared by the firmware and host_test.
// No Arduino types, no hardware.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum : int {
  PAGE_MONITOR_0 = 0,
  PAGE_MONITOR_1 = 1,
  PAGE_PET = 2,
  PAGE_LIST = 3,
};

inline bool pageIsMonitor(int page) {
  return page == PAGE_MONITOR_0 || page == PAGE_MONITOR_1;
}

// BOOT short: if already on a monitor page, toggle 0/1; otherwise go to page 0.
// Does not toggle the screen.
inline int onBootShort(int page) {
  if (page == PAGE_MONITOR_0) return PAGE_MONITOR_1;
  if (page == PAGE_MONITOR_1) return PAGE_MONITOR_0;
  return PAGE_MONITOR_0;
}

// KEY short: open the companion page; press again to toggle the thread list.
inline int onKeyShort(int page) {
  if (page == PAGE_PET) return PAGE_LIST;
  if (page == PAGE_LIST) return PAGE_PET;
  return PAGE_PET;
}

// Approved preview wording: 刚才 / N分钟 / N小时 / 昨天 / N天前.
inline void formatPetAge(char* out, size_t n, int ageS) {
  if (out == nullptr || n == 0) return;
  if (ageS < 0) ageS = 0;
  if (ageS < 60) {
    snprintf(out, n, "刚才");
  } else if (ageS < 3600) {
    snprintf(out, n, "%d分钟", ageS / 60);
  } else if (ageS < 86400) {
    snprintf(out, n, "%d小时", ageS / 3600);
  } else if (ageS < 172800) {
    snprintf(out, n, "昨天");
  } else {
    snprintf(out, n, "%d天前", ageS / 86400);
  }
}

// Matches PetSpriteId in pet_sprites.h (idle, running, running_b, waiting, review, failed).
enum PetFrame : int {
  PET_FRAME_IDLE = 0,
  PET_FRAME_RUNNING = 1,
  PET_FRAME_RUNNING_B = 2,
  PET_FRAME_WAITING = 3,
  PET_FRAME_REVIEW = 4,
  PET_FRAME_FAILED = 5,
};

// runningAlt is the second mains-only running frame. Battery stays on frame A.
inline int petFrameFor(const char* state, bool runningAlt) {
  if (state != nullptr) {
    if (strcmp(state, "needs_input") == 0) return PET_FRAME_WAITING;
    if (strcmp(state, "ready") == 0) return PET_FRAME_REVIEW;
    if (strcmp(state, "blocked") == 0) return PET_FRAME_FAILED;
    if (strcmp(state, "running") == 0) return runningAlt ? PET_FRAME_RUNNING_B : PET_FRAME_RUNNING;
  }
  return PET_FRAME_IDLE;
}

// Animate the two running frames every 2s, but only on mains and only while running.
inline bool runningAltFrame(uint32_t nowMs, bool onMains, const char* state) {
  if (!onMains) return false;
  if (state == nullptr || strcmp(state, "running") != 0) return false;
  return (nowMs / 2000u) % 2u == 1u;
}

inline const char* petLabelFor(const char* state) {
  if (state != nullptr) {
    if (strcmp(state, "needs_input") == 0) return "需要你回复";
    if (strcmp(state, "ready") == 0) return "已完成";
    if (strcmp(state, "blocked") == 0) return "出错了";
    if (strcmp(state, "running") == 0) return "运行中";
  }
  return "空闲";
}
