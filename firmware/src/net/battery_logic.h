#pragma once
// 电池电量的纯逻辑（不依赖 Arduino），固件和主机端测试共用。
// 硬件：Waveshare ESP32-S3-RLCD-4.2，18650 经 200k/100k 分压接 GPIO4（ADC1_CH3）；
// 充电芯片的 STAT 只接了板上 LED，固件读不到，所以「充电中」只能靠电压推断。
#include <stdint.h>

namespace batt {

constexpr int kMinPlausibleMv = 2000;   // 低于此（或高于下面的值）视为没接电池/读数无效，不显示图标
constexpr int kMaxPlausibleMv = 4500;
constexpr int kFullChargeMv = 4150;     // 滤波后 ≥ 此值：按「充电/已满/外接供电」处理（平台期）
constexpr int kPlateauExitMv = 4130;    // 平台期要降到这个值以下才退出（滞回，避免在 4150 附近抖动）
constexpr float kEmaAlpha = 0.3f;       // 每次更新（30s）的 EMA 权重
constexpr int kLowPercent = 15;         // ≤ 此值显示低电量反白

// 18650 静置电压 -> 百分比，分段线性插值（电压降序）。
struct Pt { int mv; int pct; };
constexpr Pt kCurve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75}, {3950, 70},
    {3910, 65},  {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45}, {3800, 40}, {3790, 35},
    {3770, 30},  {3750, 25}, {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},  {3300, 0},
};
constexpr int kCurveLen = static_cast<int>(sizeof(kCurve) / sizeof(kCurve[0]));

inline bool plausible(int mv) { return mv >= kMinPlausibleMv && mv <= kMaxPlausibleMv; }

inline int percentFromMv(int mv) {
  if (mv >= kCurve[0].mv) return 100;
  if (mv <= kCurve[kCurveLen - 1].mv) return 0;
  for (int i = 0; i + 1 < kCurveLen; i++) {
    const Pt& hi = kCurve[i];
    const Pt& lo = kCurve[i + 1];
    if (mv <= hi.mv && mv >= lo.mv) {
      const int span = hi.mv - lo.mv;
      // 四舍五入的线性插值
      return lo.pct + ((mv - lo.mv) * (hi.pct - lo.pct) + span / 2) / span;
    }
  }
  return 0;
}

// ±1% 滞回：新值与当前显示值相差不超过 1 就保持不动；shown<0 表示还没有显示值。
inline int applyHysteresis(int newPct, int shown) {
  if (shown < 0) return newPct;
  const int d = newPct - shown;
  if (d >= -1 && d <= 1) return shown;
  return newPct;
}

// 喂入「16 次采样平均后的 mV」，得到 EMA 电压和带滞回的百分比。充电推断见 ChargeDetector。
struct Tracker {
  bool has = false;
  float emaMv = 0;
  int shownPct = -1;
  bool valid = false;

  // 返回 false 表示读数不合理（图标应隐藏）。
  bool update(int avgMv) {
    if (!plausible(avgMv)) {
      valid = false;
      has = false;       // 下次合理读数重新起算
      shownPct = -1;
      return false;
    }
    if (!has) {
      emaMv = static_cast<float>(avgMv);
      has = true;
    } else {
      emaMv += kEmaAlpha * (static_cast<float>(avgMv) - emaMv);
    }
    shownPct = applyHysteresis(percentFromMv(filteredMv()), shownPct);
    valid = true;
    return true;
  }

  int filteredMv() const { return static_cast<int>(emaMv + 0.5f); }
};

// ---- 充电推断（5 分钟趋势，基于时间，与采样周期无关）----
// 充电芯片状态没接到 GPIO，只能看电压：
//   * 滤波电压 ≥4.15V（平台期，降到 4.13V 以下才退出）→ 充电（含没装电池只接 USB 的 4.2V）
//   * 否则看最近约 5 分钟的净变化（按 5 分钟折算）：> +10mV 判充电；≤ +3mV 判未充电；之间保持原状态（滞回）
//   * 窗口不足 3 分钟时不判断：保持原状态（开机为 Unknown，按市电档处理）
enum class ChargeState : uint8_t { Unknown = 0, Charging, Discharging };

constexpr uint32_t kTrendWindowMs = 300000;   // 5 分钟
constexpr uint32_t kTrendMinSpanMs = 180000;  // 至少 3 分钟的数据才下结论
constexpr int kRiseOnMv = 10;                 // 折算到 5 分钟，净上升超过它 = 充电
constexpr int kRiseOffMv = 3;                 // 折算到 5 分钟，净变化不超过它 = 未充电

struct ChargeDetector {
  static constexpr int kCap = 16;
  struct Sample { uint32_t t; int mv; };

  Sample ring[kCap];
  int head = 0;  // 最旧样本下标
  int len = 0;
  ChargeState state = ChargeState::Unknown;
  bool plateau = false;
  int lastNetScaled = 0;  // 最近一次评估的折算净变化（调试用）

  void reset() {
    head = 0;
    len = 0;
    state = ChargeState::Unknown;
    plateau = false;
    lastNetScaled = 0;
  }

  const Sample& at(int i) const { return ring[(head + i) % kCap]; }

  void push(uint32_t nowMs, int mv) {
    if (len == kCap) {  // 满了丢最旧
      head = (head + 1) % kCap;
      len--;
    }
    ring[(head + len) % kCap] = Sample{nowMs, mv};
    len++;
    // 保留「年龄 ≥ 窗口的最新样本」作为锚点，更旧的丢掉
    while (len >= 2 && static_cast<uint32_t>(nowMs - at(1).t) >= kTrendWindowMs) {
      head = (head + 1) % kCap;
      len--;
    }
  }

  // 喂入一次滤波后的电压。返回当前状态。
  ChargeState add(uint32_t nowMs, int filteredMv) {
    push(nowMs, filteredMv);
    if (filteredMv >= kFullChargeMv) plateau = true;
    else if (plateau && filteredMv < kPlateauExitMv) plateau = false;
    if (plateau) {
      state = ChargeState::Charging;
      return state;
    }
    const Sample& anchor = at(0);
    const uint32_t span = nowMs - anchor.t;
    if (len < 2 || span < kTrendMinSpanMs) return state;  // 数据不够：保持原状态
    const int64_t net = static_cast<int64_t>(filteredMv - anchor.mv) * kTrendWindowMs / static_cast<int64_t>(span);
    lastNetScaled = static_cast<int>(net);
    if (net > kRiseOnMv) state = ChargeState::Charging;
    else if (net <= kRiseOffMv) state = ChargeState::Discharging;
    return state;
  }
};

}  // namespace batt
