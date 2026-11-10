#include "battery.h"

#include <stdio.h>

#include "../state.h"
#include "battery_logic.h"
#include "power.h"

namespace {

constexpr int kBattPin = 4;                 // GPIO4 = ADC1_CH3（ADC1 在 Wi-Fi 开启时也能用）
constexpr int kDividerRatio = 3;            // 200k / 100k 分压，ADC 只看到 1/3
constexpr int kSamples = 16;

batt::Tracker tracker;
batt::ChargeDetector detector;
batt::ChargeState lastLoggedState = batt::ChargeState::Unknown;
uint32_t nextUpdateAt = 0;

bool timeReached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

// 16 次采样平均（analogReadMilliVolts 已含出厂校准），再乘分压比得到电池电压 mV。
int readBatteryMv() {
  uint32_t sum = 0;
  for (int i = 0; i < kSamples; i++) {
    sum += analogReadMilliVolts(kBattPin);
    delayMicroseconds(300);
  }
  return static_cast<int>(sum / kSamples) * kDividerRatio;
}

const char* stateName(batt::ChargeState s) {
  return s == batt::ChargeState::Charging ? "充电" : (s == batt::ChargeState::Discharging ? "放电" : "未知");
}

void update() {
  const int rawMv = readBatteryMv();
  const bool ok = tracker.update(rawMv);
  g.battValid = ok;
  if (ok) {
    g.battMv = static_cast<int16_t>(tracker.filteredMv());
    g.battPercent = static_cast<int8_t>(tracker.shownPct);
    const batt::ChargeState cs = detector.add(millis(), tracker.filteredMv());
    g.battCharging = cs == batt::ChargeState::Charging;
  } else {
    detector.reset();  // 读数不合理（没接电池）：趋势作废
    g.battMv = -1;
    g.battPercent = -1;
    g.battCharging = false;
  }
  const batt::ChargeState cs = detector.state;
  Serial.printf("[batt] 原始 %d mV  滤波 %d mV  %d%%  充电=%s  5分钟趋势 %+d mV  有效=%d\n", rawMv,
                ok ? tracker.filteredMv() : -1, ok ? tracker.shownPct : -1, stateName(cs), detector.lastNetScaled,
                ok ? 1 : 0);
  if (cs != lastLoggedState) {
    Serial.printf("[power] 充电状态：%s -> %s\n", stateName(lastLoggedState), stateName(cs));
    lastLoggedState = cs;
  }
}

}  // namespace

void battBegin() {
  // ESP32-S3 的 ADC_11db 即 12dB 档（约 0–3.1V 量程）：满电 4.2V/3 = 1.4V，必须用它，否则读数卡在最大值。
  analogSetPinAttenuation(kBattPin, ADC_11db);
  update();  // 开机立即读一次，图标不用等 30 秒
  nextUpdateAt = millis() + powerIv().battUpdateMs;
}

void battLoop() {
  const uint32_t now = millis();
  if (!timeReached(now, nextUpdateAt)) return;
  nextUpdateAt = now + powerIv().battUpdateMs;
  update();
}

void battKick() { nextUpdateAt = millis(); }

batt::ChargeState battChargeState() { return detector.state; }
