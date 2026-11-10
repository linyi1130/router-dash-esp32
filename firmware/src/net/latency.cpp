#include "latency.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <stdio.h>
#include <string.h>

#include "../state.h"
#include "latency_logic.h"
#include "power.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy firmware/include/config.example.h to firmware/include/config.h before building."
#endif

// 延迟不再自己连站点做 TLS（ESP32 软件 TLS 经代理常超过 2 秒必超时），
// 而是让 Clash 测：GET /proxies/<代理>/delay?url=<站点>&timeout=5000，与 zashboard 一致。
// 测速在独立的 FreeRTOS 任务里跑，主循环/界面/UPnP 不会被它阻塞。
namespace {

struct LatencySite {
  const char* name;
  const char* url;
  bool direct;  // true：走 DIRECT（国内站点，规则里直连）；false：走节点选择组（当前所选节点）
};

constexpr LatencySite kSites[kLatencySites] = {
    {"Baidu", "http://www.baidu.com/", true},
    {"CF", "https://cp.cloudflare.com/", false},
    {"OpenAI", "https://cdn.openai.com/", false},
    {"Github", "https://github.com/", false},
    {"YouTube", "https://www.youtube.com/", false},
};

constexpr uint32_t kClashTimeoutMs = 5000;     // 传给 Clash 的 timeout 参数
constexpr uint16_t kConnectTimeoutMs = 1000;   // 到 Clash 面板（局域网）的连接超时
constexpr uint16_t kReadTimeoutMs = 7000;      // Clash 最多 5s，加余量（只在后台任务里等）
constexpr uint32_t kGapBetweenSitesMs = 200;
constexpr uint32_t kMinCycleMs = 5000;
constexpr uint32_t kTaskStack = 10240;

TaskHandle_t taskHandle = nullptr;

int measureSite(const LatencySite& s) {
  char path[384];
  const char* proxy = s.direct ? "DIRECT" : PROXY_GROUP;
  if (!latency::buildDelayPath(proxy, s.url, kClashTimeoutMs, path, sizeof(path))) return -1;
  HTTPClient http;
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kReadTimeoutMs);
  http.setReuse(false);
  if (!http.begin(CLASH_HOST, CLASH_PORT, path)) return -1;
  http.addHeader("Authorization", String("Bearer ") + CLASH_SECRET);
  const int code = http.GET();
  String body;
  if (code == 200) body = http.getString();
  http.end();
  return latency::parseDelayMs(code, body.c_str());
}

void latencyTask(void*) {
  for (;;) {
    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t cycleStart = millis();
    for (int i = 0; i < kLatencySites; i++) {
      if (WiFi.status() != WL_CONNECTED) break;
      g.latency[i] = static_cast<int16_t>(measureSite(kSites[i]));  // 逐个写入，屏幕逐步补全
      vTaskDelay(pdMS_TO_TICKS(kGapBetweenSitesMs));
    }
    // 周期随市电/电池档变化；latencyKick() 可提前唤醒（切回市电档时立即测一轮）。
    uint32_t interval = powerIv().latencyCycleMs;
    if (interval < kMinCycleMs) interval = kMinCycleMs;
    const uint32_t elapsed = millis() - cycleStart;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(elapsed < interval ? interval - elapsed : kMinCycleMs));
  }
}

}  // namespace

void latencyBegin() {
  if (taskHandle) return;  // 只建一次；WiFi 断开时任务自己等待
  xTaskCreatePinnedToCore(latencyTask, "latency", kTaskStack, nullptr, 1, &taskHandle, 0);
}

void latencyKick() {
  if (taskHandle) xTaskNotifyGive(taskHandle);
}

void latencyLoop() {}  // 保留接口：测速已在后台任务里，主循环无需做事
