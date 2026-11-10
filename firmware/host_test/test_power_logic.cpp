// g++ -std=c++17 -Wall -Wextra -I../src/net test_power_logic.cpp -o /tmp/test_power_logic
#include <stdio.h>

#include "power_logic.h"

static int checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
  using namespace power;
  using batt::ChargeState;

  // 目标档位
  CHECK(targetFor(ChargeState::Unknown, true) == Profile::Mains);        // 开机/数据不足 = 市电档
  CHECK(targetFor(ChargeState::Charging, true) == Profile::Mains);
  CHECK(targetFor(ChargeState::Discharging, true) == Profile::Battery);
  CHECK(targetFor(ChargeState::Discharging, false) == Profile::Mains);   // 读不到电池（没装电池接 USB）= 市电

  // 周期表合理性：电池档都不比市电档快；过期阈值与采样周期成比例
  CHECK(kBattery.upnpSampleMs >= kMains.upnpSampleMs && kBattery.latencyCycleMs >= kMains.latencyCycleMs);
  CHECK(kBattery.codexPollMs >= kMains.codexPollMs && kBattery.clashConnectionsMs >= kMains.clashConnectionsMs);
  CHECK(kBattery.ipinfoRefreshMs >= kMains.ipinfoRefreshMs && kBattery.battUpdateMs >= kMains.battUpdateMs);
  CHECK(kBattery.redrawMs >= kMains.redrawMs);
  CHECK(kBattery.upnpStaleMs >= kBattery.upnpSampleMs * 5 / 2);          // ≥ 2.5 倍
  CHECK(kMains.upnpStaleMs >= kMains.upnpSampleMs * 5 / 2);
  CHECK(kBattery.codexStaleScale * kMains.codexPollMs >= kBattery.codexPollMs);          // 阈值放宽倍数 ≥ 轮询放慢倍数
  CHECK(kBattery.codexBackoffMaxMs >= kBattery.codexPollMs);
  CHECK(kMains.upnpSampleMs == 2000 && kMains.latencyCycleMs == 60000 && kMains.codexPollMs == 5000);   // 市电档 = 之前的值
  CHECK(kBattery.upnpSampleMs == 10000 && kBattery.latencyCycleMs == 300000 && kBattery.codexPollMs == 30000);
  CHECK(kBattery.clashConnectionsMs == 30000 && kBattery.ipinfoRefreshMs == 600000);
  CHECK(kBattery.battUpdateMs == 60000 && kBattery.redrawMs == 5000);
  CHECK(&intervalsFor(Profile::Mains) == &kMains && &intervalsFor(Profile::Battery) == &kBattery);

  // 开机：市电档；一直 Unknown 永不切换
  {
    ProfileSelector s;
    CHECK(s.active == Profile::Mains);
    for (uint32_t t = 0; t < 600000; t += 1000) CHECK(!s.update(t, ChargeState::Unknown, true));
    CHECK(s.active == Profile::Mains);
  }
  // 放电需稳定 60 秒才切到电池档
  {
    ProfileSelector s;
    CHECK(!s.update(1000, ChargeState::Discharging, true));
    CHECK(!s.update(30000, ChargeState::Discharging, true));
    CHECK(!s.update(60999, ChargeState::Discharging, true));            // 59.999s
    CHECK(s.update(61000, ChargeState::Discharging, true));             // 60s
    CHECK(s.active == Profile::Battery);
    CHECK(!s.update(62000, ChargeState::Discharging, true));            // 只切换一次
  }
  // 目标来回翻转（<60s）不切换
  {
    ProfileSelector s;
    uint32_t t = 0;
    for (int i = 0; i < 20; i++) {
      s.update(t, ChargeState::Discharging, true); t += 30000;
      s.update(t, ChargeState::Charging, true); t += 30000;
    }
    CHECK(s.active == Profile::Mains);
  }
  // 电池档 -> 市电：同样需稳定 60s
  {
    ProfileSelector s;
    uint32_t t = 0;
    s.update(t, ChargeState::Discharging, true); t += 60000;
    CHECK(s.update(t, ChargeState::Discharging, true) && s.active == Profile::Battery);
    t += 1000;
    CHECK(!s.update(t, ChargeState::Charging, true));
    t += 59000;
    CHECK(!s.update(t, ChargeState::Charging, true));
    t += 1000;
    CHECK(s.update(t, ChargeState::Charging, true) && s.active == Profile::Mains);
  }
  // 电池档下读数变得不合理（拔电池/USB 直供）-> 市电
  {
    ProfileSelector s;
    uint32_t t = 0;
    s.update(t, ChargeState::Discharging, true); t += 60000;
    s.update(t, ChargeState::Discharging, true);
    CHECK(s.active == Profile::Battery);
    s.update(t + 1000, ChargeState::Discharging, false);
    CHECK(s.update(t + 61000, ChargeState::Discharging, false) && s.active == Profile::Mains);
  }
  // millis() 回绕
  {
    ProfileSelector s;
    uint32_t t = 0xFFFFFFFFu - 30000u;
    s.update(t, ChargeState::Discharging, true);
    CHECK(!s.update(t + 30000u, ChargeState::Discharging, true));
    CHECK(s.update(t + 60000u, ChargeState::Discharging, true));        // 跨过回绕点
  }
  // 与充电检测联动：平台期（充满后仍插着）= 市电档；拔掉后下降 -> 电池档
  {
    batt::ChargeDetector d;
    ProfileSelector s;
    uint32_t t = 0;
    for (int i = 0; i < 20; i++) { d.add(t, 4190); s.update(t, d.state, true); t += 30000; }
    CHECK(d.state == ChargeState::Charging && s.active == Profile::Mains);
    for (int i = 0; i < 40; i++) { d.add(t, 4100 - i); s.update(t, d.state, true); t += 30000; }
    CHECK(d.state == ChargeState::Discharging && s.active == Profile::Battery);
  }

  printf("OK: %d checks passed\n", checks);
  return 0;
}
