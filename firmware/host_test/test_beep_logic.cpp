// g++ -std=c++17 -Wall -Wextra -I../src test_beep_logic.cpp -o /tmp/test_beep_logic
#include <stdio.h>

#include "audio/beep_logic.h"

static int checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
  CHECK(!shouldBeepOnCodexStatus(false, "running", "complete"));   // 开机/重连后的第一帧
  CHECK(!shouldBeepOnCodexStatus(false, "", "complete"));
  CHECK(shouldBeepOnCodexStatus(true, "running", "complete"));
  CHECK(!shouldBeepOnCodexStatus(true, "running", "interrupted"));
  CHECK(!shouldBeepOnCodexStatus(true, "running", "running"));
  CHECK(!shouldBeepOnCodexStatus(true, "running", "idle"));
  CHECK(!shouldBeepOnCodexStatus(true, "complete", "complete"));     // 切到已经完成的窗口
  CHECK(!shouldBeepOnCodexStatus(true, "idle", "complete"));
  CHECK(!shouldBeepOnCodexStatus(true, "interrupted", "complete"));
  CHECK(!shouldBeepOnCodexStatus(true, "complete", "running"));
  CHECK(!shouldBeepOnCodexStatus(true, nullptr, "complete"));
  CHECK(!shouldBeepOnCodexStatus(true, "running", nullptr));

  CHECK(codexPollMsForStatus("running", 5000) == 1000);
  CHECK(codexPollMsForStatus("running", 30000) == 1000);
  CHECK(codexPollMsForStatus("running", 500) == 500);              // 已更短则保持
  CHECK(codexPollMsForStatus("complete", 5000) == 5000);
  CHECK(codexPollMsForStatus("idle", 30000) == 30000);
  CHECK(codexPollMsForStatus(nullptr, 5000) == 5000);

  printf("OK: %d checks passed\n", checks);
  return 0;
}
