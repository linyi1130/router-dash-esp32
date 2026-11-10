#include <Arduino.h>
#include <WiFi.h>

#include <stdio.h>

#include "net/battery.h"
#include "net/clash_client.h"
#include "net/codex_client.h"
#include "net/latency.h"
#include "net/power.h"
#include "net/upnp_wan.h"
#include "net/ipinfo_client.h"
#include "state.h"
#include "ui/dashboard.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy firmware/include/config.example.h to firmware/include/config.h before building."
#endif

namespace {
constexpr uint32_t kWifiTimeoutMs = 15000;
constexpr uint32_t kWifiRetryMs = 30000;
constexpr uint32_t kHeapLogMs = 600000;
constexpr uint32_t kStatusLogMs = 10000;

uint32_t lastWifiRetry = 0;
uint32_t lastHeapLog = 0;
uint32_t lastStatusLog = 0;
bool wifiWasUp = false;
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.println("router-dash-esp32 启动");

  uiBegin();
  powerBegin();
  battBegin();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  WiFi.setHostname("router-dash");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("连接 Wi-Fi %s\n", WIFI_SSID);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kWifiTimeoutMs) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("已连接 %s\n", WiFi.localIP().toString().c_str());
    g.link = LINK_OK;
    wifiWasUp = true;
    clashBegin();
    upnpBegin();
    ipinfoBegin();
    codexBegin();
  } else {
    Serial.println("Wi-Fi 未连接");
    g.link = LINK_WIFI;
    lastWifiRetry = millis();
  }
  latencyBegin();
  g.lastSampleMs = millis();
  lastHeapLog = millis();
}

void loop() {
  const uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (g.link != LINK_WIFI) {
      g.link = LINK_WIFI;
      clashDisconnect();
      upnpDisconnect();
      codexDisconnect();
    }
    if (now - lastWifiRetry >= kWifiRetryMs) {
      lastWifiRetry = now;
      Serial.println("重试 Wi-Fi");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    battLoop();
    powerLoop();
    uiLoop();
    delay(20);
    return;
  }

  if (!wifiWasUp) {
    wifiWasUp = true;
    Serial.printf("已连接 %s\n", WiFi.localIP().toString().c_str());
    clashBegin();
    upnpBegin();
    ipinfoBegin();
    codexBegin();
  }

  clashLoop();
  latencyLoop();
  upnpLoop();
  ipinfoLoop();
  codexLoop();
  battLoop();
  powerLoop();

  if (now - g.lastSampleMs >= 1000) {
    g.lastSampleMs = now;
    pushHistory(g.downSpeed, g.upSpeed);
  }

  if (now - lastHeapLog >= kHeapLogMs) {
    lastHeapLog = now;
    Serial.printf("free heap: %u bytes\n", static_cast<unsigned>(ESP.getFreeHeap()));
  }

  if (now - lastStatusLog >= kStatusLogMs) {
    lastStatusLog = now;
    Serial.printf("down=%lld up=%lld conn=%u mem=%lld downTotal=%lld upTotal=%lld node=%s lat=%d,%d,%d,%d,%d\n",
                  static_cast<long long>(g.downSpeed), static_cast<long long>(g.upSpeed),
                  g.connCount, static_cast<long long>(g.memInuse),
                  static_cast<long long>(g.downloadTotal), static_cast<long long>(g.uploadTotal),
                  g.currentNode,
                  static_cast<int>(g.latency[0]), static_cast<int>(g.latency[1]),
                  static_cast<int>(g.latency[2]), static_cast<int>(g.latency[3]),
                  static_cast<int>(g.latency[4]));
  }

  uiLoop();
  delay(20);
}
