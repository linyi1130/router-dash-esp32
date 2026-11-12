// g++ -std=c++17 -Wall -Wextra -I../src/ui test_page_logic.cpp -o /tmp/test_page_logic
#include <stdio.h>
#include <string.h>

#include "page_logic.h"

static int checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int eq(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

int main() {
  CHECK(pageIsMonitor(PAGE_MONITOR_0));
  CHECK(pageIsMonitor(PAGE_MONITOR_1));
  CHECK(!pageIsMonitor(PAGE_PET));
  CHECK(!pageIsMonitor(PAGE_LIST));

  // BOOT short: toggle the two monitor pages; from pet/list jump to monitor 0.
  CHECK(onBootShort(PAGE_MONITOR_0) == PAGE_MONITOR_1);
  CHECK(onBootShort(PAGE_MONITOR_1) == PAGE_MONITOR_0);
  CHECK(onBootShort(PAGE_PET) == PAGE_MONITOR_0);
  CHECK(onBootShort(PAGE_LIST) == PAGE_MONITOR_0);
  CHECK(onBootShort(99) == PAGE_MONITOR_0);

  // KEY short: go to pet, then toggle pet/list. From monitor, open pet.
  CHECK(onKeyShort(PAGE_MONITOR_0) == PAGE_PET);
  CHECK(onKeyShort(PAGE_MONITOR_1) == PAGE_PET);
  CHECK(onKeyShort(PAGE_PET) == PAGE_LIST);
  CHECK(onKeyShort(PAGE_LIST) == PAGE_PET);

  char buf[16];
  formatPetAge(buf, sizeof(buf), 0);
  CHECK(eq(buf, "刚才"));
  formatPetAge(buf, sizeof(buf), 59);
  CHECK(eq(buf, "刚才"));
  formatPetAge(buf, sizeof(buf), 60);
  CHECK(eq(buf, "1分钟"));
  formatPetAge(buf, sizeof(buf), 3 * 60 + 20);
  CHECK(eq(buf, "3分钟"));
  formatPetAge(buf, sizeof(buf), 3600);
  CHECK(eq(buf, "1小时"));
  formatPetAge(buf, sizeof(buf), 5 * 3600 + 10);
  CHECK(eq(buf, "5小时"));
  formatPetAge(buf, sizeof(buf), 86400);
  CHECK(eq(buf, "昨天"));
  formatPetAge(buf, sizeof(buf), 172800 - 1);
  CHECK(eq(buf, "昨天"));
  formatPetAge(buf, sizeof(buf), 172800);
  CHECK(eq(buf, "2天前"));
  formatPetAge(buf, sizeof(buf), -5);
  CHECK(eq(buf, "刚才"));

  CHECK(petFrameFor("idle", false) == PET_FRAME_IDLE);
  CHECK(petFrameFor("idle", true) == PET_FRAME_IDLE);
  CHECK(petFrameFor("needs_input", false) == PET_FRAME_WAITING);
  CHECK(petFrameFor("ready", true) == PET_FRAME_REVIEW);
  CHECK(petFrameFor("blocked", false) == PET_FRAME_FAILED);
  CHECK(petFrameFor("running", false) == PET_FRAME_RUNNING);
  CHECK(petFrameFor("running", true) == PET_FRAME_RUNNING_B);
  CHECK(petFrameFor(nullptr, true) == PET_FRAME_IDLE);
  CHECK(petFrameFor("unknown", true) == PET_FRAME_IDLE);

  // Mains running: odd 2s windows use frame B; battery never alternates.
  CHECK(runningAltFrame(0, true, "running") == false);
  CHECK(runningAltFrame(1999, true, "running") == false);
  CHECK(runningAltFrame(2000, true, "running") == true);
  CHECK(runningAltFrame(3999, true, "running") == true);
  CHECK(runningAltFrame(4000, true, "running") == false);
  CHECK(runningAltFrame(2000, false, "running") == false);
  CHECK(runningAltFrame(2000, true, "idle") == false);
  CHECK(runningAltFrame(2000, true, "needs_input") == false);
  CHECK(runningAltFrame(2000, true, nullptr) == false);

  CHECK(eq(petLabelFor("needs_input"), "需要你回复"));
  CHECK(eq(petLabelFor("ready"), "已完成"));
  CHECK(eq(petLabelFor("blocked"), "出错了"));
  CHECK(eq(petLabelFor("running"), "运行中"));
  CHECK(eq(petLabelFor("idle"), "空闲"));
  CHECK(eq(petLabelFor(nullptr), "空闲"));

  printf("ok %d checks\n", checks);
  return 0;
}
