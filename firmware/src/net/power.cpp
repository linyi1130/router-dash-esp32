#include "power.h"

#include <stdio.h>

#include "../state.h"
#include "battery.h"
#include "clash_client.h"
#include "codex_client.h"
#include "ipinfo_client.h"
#include "latency.h"
#include "upnp_wan.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy firmware/include/config.example.h to firmware/include/config.h before building."
#endif

namespace {

power::ProfileSelector selector;
power::Intervals mains = power::kMains;  // 市电档：默认值被 config.h 的可选宏覆盖
power::Intervals battery = power::kBattery;
bool initialized = false;

void initIntervals() {
  if (initialized) return;
  initialized = true;
#ifdef UPNP_SAMPLE_MS
  mains.upnpSampleMs = UPNP_SAMPLE_MS;
  mains.upnpStaleMs = UPNP_SAMPLE_MS * 5 > 10000 ? UPNP_SAMPLE_MS * 5 : 10000;
#endif
#ifdef CODEX_POLL_MS
  mains.codexPollMs = CODEX_POLL_MS;
#endif
#ifdef LATENCY_INTERVAL_MS
  mains.latencyCycleMs = LATENCY_INTERVAL_MS;
#endif
}

}  // namespace

const power::Intervals& powerIv() {
  initIntervals();
  return g.powerBattery ? battery : mains;
}

void powerBegin() {
  initIntervals();
  selector = power::ProfileSelector();
  g.powerBattery = false;  // 开机默认市电档
}

void powerLoop() {
  const uint32_t now = millis();
  if (!selector.update(now, battChargeState(), g.battValid)) return;
  const bool toBattery = selector.active == power::Profile::Battery;
  g.powerBattery = toBattery;
  const power::Intervals& iv = powerIv();
  Serial.printf("[power] 切换到%s档：UPnP %lus 延迟 %lus Codex %lus 重绘 %lus\n", toBattery ? "电池" : "市电",
                static_cast<unsigned long>(iv.upnpSampleMs / 1000), static_cast<unsigned long>(iv.latencyCycleMs / 1000),
                static_cast<unsigned long>(iv.codexPollMs / 1000), static_cast<unsigned long>(iv.redrawMs / 1000));
  if (!toBattery) {  // 切回市电：各数据源立即刷新一次，不等旧的长周期
    upnpKick();
    codexKick();
    clashKick();
    ipinfoKick();
    latencyKick();
    battKick();
  }
}
