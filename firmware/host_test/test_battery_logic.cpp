// g++ -std=c++17 -Wall -Wextra -I../src/net test_battery_logic.cpp -o /tmp/test_battery_logic
#include <stdio.h>

#include "battery_logic.h"

static int checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
  using namespace batt;

  // 曲线端点与表内点
  CHECK(percentFromMv(4200) == 100 && percentFromMv(4300) == 100);
  CHECK(percentFromMv(4150) == 95 && percentFromMv(3840) == 50 && percentFromMv(3710) == 15);
  CHECK(percentFromMv(3300) == 0 && percentFromMv(3000) == 0);
  // 插值：4150~4110 之间 4130 -> 92.5 -> 93（四舍五入）；3610~3300 之间 3455 -> 2.5 -> 3
  CHECK(percentFromMv(4130) == 93);
  CHECK(percentFromMv(3455) == 3);
  // 单调：电压越高百分比不降
  int prev = -1;
  for (int mv = 3000; mv <= 4300; mv++) { const int p = percentFromMv(mv); CHECK(p >= prev && p >= 0 && p <= 100); prev = p; }
  // 曲线本身严格降序、百分比降序
  for (int i = 0; i + 1 < kCurveLen; i++) CHECK(kCurve[i].mv > kCurve[i + 1].mv && kCurve[i].pct > kCurve[i + 1].pct);

  // 合理性
  CHECK(!plausible(1999) && plausible(2000) && plausible(4500) && !plausible(4501) && !plausible(0));

  // ±1% 滞回
  CHECK(applyHysteresis(50, -1) == 50);
  CHECK(applyHysteresis(51, 50) == 50 && applyHysteresis(49, 50) == 50 && applyHysteresis(50, 50) == 50);
  CHECK(applyHysteresis(52, 50) == 52 && applyHysteresis(48, 50) == 48);

  // 追踪器：首次读数直接采用；抖动不改变显示值
  {
    Tracker t;
    CHECK(t.update(3840) && t.valid && t.shownPct == 50);
    CHECK(t.update(3843) && t.shownPct == 50);
    CHECK(t.update(3836) && t.shownPct == 50);
  }
  // 放电：EMA 平滑，缓慢下降
  {
    Tracker t;
    t.update(3900);
    for (int i = 0; i < 20; i++) t.update(3750);
    CHECK(t.filteredMv() > 3745 && t.filteredMv() < 3760);
    CHECK(t.shownPct >= 24 && t.shownPct <= 26);
  }
  // 不合理读数：隐藏并复位；合理读数恢复后重新起算
  {
    Tracker t;
    t.update(3900);
    CHECK(!t.update(1500) && !t.valid && t.shownPct == -1);
    CHECK(!t.update(5000) && !t.valid);
    CHECK(t.update(3900) && t.valid && t.shownPct == percentFromMv(3900));
    CHECK(!t.update(0));
  }

  // ---- 充电推断（5 分钟趋势，时间驱动）----
  // 用真实日志的斜率：充电中约 3.8V，2 分钟涨 7mV（≈3.5mV/分钟），30s 周期。
  {
    ChargeDetector d;
    uint32_t t = 1000;
    int mv = 3795;
    CHECK(d.state == ChargeState::Unknown);
    d.add(t, mv);                                      // 第 0 分钟
    for (int i = 1; i <= 5; i++) { t += 30000; mv += 0; d.add(t, mv + (i * 175) / 100); }  // 到 2.5 分钟，窗口 <3 分钟
    CHECK(d.state == ChargeState::Unknown);            // 数据不足 3 分钟不下结论
    for (int i = 6; i <= 8; i++) { t += 30000; d.add(t, mv + (i * 175) / 100); }            // 到 4 分钟
    CHECK(d.state == ChargeState::Charging);           // 折算 5 分钟净上升 >10mV
    CHECK(d.lastNetScaled > kRiseOnMv);
  }
  // 同样的斜率，60s 周期（电池档）也能判出来，窗口仍约 5 分钟
  {
    ChargeDetector d;
    uint32_t t = 0;
    for (int i = 0; i <= 5; i++) { d.add(t, 3795 + (i * 350) / 100); t += 60000; }          // 0..5 分钟，每分钟 +3.5mV
    CHECK(d.state == ChargeState::Charging);
    CHECK(d.len <= 8);                                  // 环形缓冲没膨胀
  }
  // 平稳/放电：判为未充电
  {
    ChargeDetector d;
    uint32_t t = 0;
    for (int i = 0; i <= 8; i++) { d.add(t, 3800 - i); t += 30000; }                         // 缓慢下降
    CHECK(d.state == ChargeState::Discharging);
  }
  {
    ChargeDetector d;
    uint32_t t = 0;
    for (int i = 0; i <= 8; i++) { d.add(t, 3800); t += 30000; }
    CHECK(d.state == ChargeState::Discharging);
  }
  // 滞回：折算净变化在 (3,10] 之间保持原状态，不抖动
  {
    ChargeDetector d;                                   // 充电 -> 变慢但仍 +6mV/5min：保持充电
    uint32_t t = 0;
    for (int i = 0; i <= 10; i++) { d.add(t, 3800 + i * 4); t += 30000; }
    CHECK(d.state == ChargeState::Charging);
    int base = 3800 + 10 * 4;
    for (int i = 1; i <= 20; i++) { d.add(t, base + (i * 6) / 10); t += 30000; }             // 约 +1.2mV/分钟 = +6/5分钟
    CHECK(d.state == ChargeState::Charging);
    for (int i = 0; i <= 20; i++) { d.add(t, base + 12); t += 30000; }                       // 完全持平 -> 退出
    CHECK(d.state == ChargeState::Discharging);
  }
  {
    ChargeDetector d;                                   // 放电 -> 轻微回升 +6mV/5min：保持放电，不翻转
    uint32_t t = 0;
    for (int i = 0; i <= 10; i++) { d.add(t, 3800); t += 30000; }
    CHECK(d.state == ChargeState::Discharging);
    for (int i = 1; i <= 20; i++) { d.add(t, 3800 + (i * 6) / 10); t += 30000; }
    CHECK(d.state == ChargeState::Discharging);
  }
  // 平台期：≥4.15V 立即判充电（含没装电池接 USB 的 ~4.2V），降到 4.13V 以下才退出
  {
    ChargeDetector d;
    d.add(0, 4200);
    CHECK(d.state == ChargeState::Charging);
    d.add(30000, 4140);                                 // 4.13~4.15 之间：滞回，仍判充电
    CHECK(d.state == ChargeState::Charging);
    d.add(60000, 4120);                                 // 退出平台期，但窗口 <3 分钟：保持
    CHECK(d.state == ChargeState::Charging);
    for (int i = 3; i <= 12; i++) d.add(i * 30000u, 4120 - i);
    CHECK(d.state == ChargeState::Discharging);         // 平台后在下降
  }
  // millis() 回绕：时间差仍正确
  {
    ChargeDetector d;
    uint32_t t = 0xFFFFFFFFu - 90000u;
    for (int i = 0; i <= 8; i++) { d.add(t, 3795 + i * 2); t += 30000; }                     // 跨过回绕点，+2mV/30s
    CHECK(d.state == ChargeState::Charging);
  }
  // 窗口只保留约 5 分钟：5 分钟前的旧数据不影响现在（先涨后平，最终判未充电）
  {
    ChargeDetector d;
    uint32_t t = 0;
    for (int i = 0; i <= 10; i++) { d.add(t, 3700 + i * 10); t += 30000; }                   // 前 5 分钟快速上升
    CHECK(d.state == ChargeState::Charging);
    for (int i = 0; i <= 14; i++) { d.add(t, 3800); t += 30000; }                            // 之后 7 分钟持平
    CHECK(d.state == ChargeState::Discharging);
  }
  // reset
  {
    ChargeDetector d;
    d.add(0, 4200);
    d.reset();
    CHECK(d.state == ChargeState::Unknown && d.len == 0 && !d.plateau);
  }

  // 满电 18650：ADC 看到 1/3，乘 3 后落在 4.2V 附近
  CHECK(percentFromMv(1400 * 3) == 100);

  printf("OK: %d checks passed\n", checks);
  return 0;
}
