#pragma once
// 市电 / 电池两档刷新频率及切换逻辑的纯逻辑（不依赖 Arduino），固件和主机端测试共用。
#include <stdint.h>

#include "battery_logic.h"

namespace power {

enum class Profile : uint8_t { Mains = 0, Battery };

// 各数据源的周期。固件在 power.cpp 里用 config.h 的可选宏覆盖「市电档」的默认值。
struct Intervals {
  uint32_t upnpSampleMs;       // UPnP 外网速度采样
  uint32_t upnpStaleMs;        // 多久没有有效样本就把外网清零（≥ 2.5 × 采样周期）
  uint32_t latencyCycleMs;     // 延迟一轮的周期
  uint32_t codexPollMs;        // Codex/Grok 拉取
  uint32_t codexBackoffMaxMs;  // 拉取失败的退避上限
  uint32_t codexStaleScale;    // 「连接中断」阈值 = CODEX_STALE_MS × 此倍数
  uint32_t clashConnectionsMs; // Clash /connections
  uint32_t ipinfoRefreshMs;    // 国内/全球 IP 刷新
  uint32_t battUpdateMs;       // 电池采样
  uint32_t redrawMs;           // 屏幕定时重绘（换页/链路状态变化仍会立即重绘）
};

constexpr Intervals kMains = {2000, 10000, 60000, 5000, 30000, 1, 5000, 60000, 30000, 1000};
constexpr Intervals kBattery = {10000, 25000, 300000, 30000, 120000, 6, 30000, 600000, 60000, 5000};

// 切换前目标档位必须稳定这么久，避免电压抖动或短暂状态变化导致来回切换。
constexpr uint32_t kSwitchStableMs = 60000;

// 目标档位：没有电池（读数不合理）、充电推断未知（开机）或在充电 → 市电档；确认在放电 → 电池档。
// 注意：平台期（≥4.15V，含满电后仍插着充电线）在 ChargeDetector 里就是 Charging，所以按市电档处理。
inline Profile targetFor(batt::ChargeState cs, bool batteryValid) {
  if (!batteryValid) return Profile::Mains;
  return cs == batt::ChargeState::Discharging ? Profile::Battery : Profile::Mains;
}

struct ProfileSelector {
  Profile active = Profile::Mains;  // 开机默认市电档
  Profile candidate = Profile::Mains;
  bool hasCandidate = false;
  uint32_t candidateSince = 0;

  // 每个主循环调用一次。档位发生切换时返回 true。
  bool update(uint32_t nowMs, batt::ChargeState cs, bool batteryValid) {
    const Profile target = targetFor(cs, batteryValid);
    if (target == active) {
      hasCandidate = false;
      return false;
    }
    if (!hasCandidate || candidate != target) {
      candidate = target;
      candidateSince = nowMs;
      hasCandidate = true;
      return false;
    }
    if (static_cast<uint32_t>(nowMs - candidateSince) >= kSwitchStableMs) {
      active = target;
      hasCandidate = false;
      return true;
    }
    return false;
  }
};

inline const Intervals& intervalsFor(Profile p) { return p == Profile::Battery ? kBattery : kMains; }

}  // namespace power
