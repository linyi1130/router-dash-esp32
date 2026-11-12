#pragma once
#include <Arduino.h>

enum LinkState : uint8_t {
  LINK_WIFI = 0,  // 正在连接/无线断开
  LINK_401,       // API 密码错误
  LINK_DOWN,      // API 不可用 / 超时
  LINK_OK,
};

// Codex 面板（来自 Mac 上的 codex-desk-status sidecar）。
enum CodexLink : uint8_t {
  CODEX_NEVER = 0,  // 从未连上 sidecar
  CODEX_OK,
  CODEX_STALE,      // 曾经连上，现在超时未更新（保留上次数据）
};

// Grok Bot 用量（sidecar 的 grok 段）。
enum GrokState : uint8_t {
  GROK_UNKNOWN = 0,  // sidecar 没给 grok 段（旧版本）
  GROK_OK,
  GROK_CLOSED,       // Grok Bot 应用未运行（可能仍有缓存用量）
  GROK_UNAVAILABLE,  // 读不到（未登录/格式不认识）
};

struct CodexThread {
  char id[40] = "";
  char state[16] = "";
  char title[104] = "";
  int32_t ageS = 0;
};

struct CodexState {
  bool authFailed = false;      // sidecar 返回 401（令牌不对）
  bool everSynced = false;
  uint32_t lastOkMs = 0;        // 最近一次成功拉取的 millis()
  uint32_t syncMs = 0;          // unixBase 对应的 millis()
  int64_t unixBase = 0;         // sidecar 报告的 unix 时间
  int32_t tzOffset = 0;

  bool hasSession = false;
  bool quota = false;           // 额度将尽（label 以「额度将尽」开头，或 status 为 quota）
  char status[16] = "";         // running/complete/interrupted/idle/quota
  char label[48] = "";
  char window[48] = "";         // 侧边栏标题，没有则项目名
  char footer[64] = "";         // 约 N 天后重置 + 套餐
  char ctxText[24] = "";        // 349k / 475k
  float weekly = -1.0f;         // 本周额度百分比，<0 未知
  float context = -1.0f;        // 上下文百分比，<0 未知
  int32_t elapsedSeconds = 0;
  int64_t turnStartedAt = 0;

  GrokState grokState = GROK_UNKNOWN;
  float grokPercent = -1.0f;    // 最近一次读到的周用量，<0 未知
  int64_t grokResetUnix = 0;    // 0 = 未知

  // 伴侣页（sidecar 新增的只读摘要；缺省时当 idle）。
  char petState[16] = "";       // needs_input/blocked/ready/running/idle
  char petLabel[24] = "";
  char petTitle[104] = "";      // 最多约 32 个 UTF-8 字符
  char petThreadId[40] = "";
  int32_t petAgeS = 0;
  int16_t countNeedsInput = 0;
  int16_t countBlocked = 0;
  int16_t countReady = 0;
  int16_t countRunning = 0;
  int16_t countIdle = 0;
  CodexThread threads[8];
  int8_t threadCount = 0;
};

struct TopHost {
  char host[48];
  char srcIP[24];
  char node[48];
  int64_t bytes;
};

// 5 个延迟站点，-1 超时，-2 未测。
constexpr int kLatencySites = 5;

struct DashState {
  LinkState link = LINK_WIFI;

  int64_t downSpeed = 0;  // B/s（Clash 代理流量）
  int64_t upSpeed = 0;
  int64_t wanDownSpeed = 0;  // B/s（路由器 WAN 口总流量，UPnP IGD；wanValid=false 时为 0）
  int64_t wanUpSpeed = 0;
  bool wanValid = false;
  int64_t memInuse = 0;  // bytes
  uint32_t connCount = 0;
  int64_t downloadTotal = 0;  // bytes
  int64_t uploadTotal = 0;

  char version[16] = "";
  char currentNode[64] = "";

  // 网络信息（第二页）：国内出口 / 全球节点出口。
  char chinaLoc[64] = "";    // 归属地，如 "中国 某省 某市 某运营商"
  char chinaIp[48] = "";     // 国内出口 IP
  char globalLoc[64] = "";   // 归属地，如 "United States GorillaServers"
  char globalIp[48] = "";    // 全球节点出口 IP

  TopHost topDown;
  TopHost topUp;

  int16_t latency[kLatencySites] = {-2, -2, -2, -2, -2};

  // 60 秒速度历史（环形缓冲）。
  int64_t histDown[60];
  int64_t histUp[60];
  int histLen = 0;
  int histHead = 0;

  CodexState codex;

  // 电池（GPIO4 ADC，见 net/battery.cpp）。battValid=false 时不显示图标。
  bool battValid = false;
  int16_t battMv = -1;       // 滤波后的电池电压 mV
  int8_t battPercent = -1;   // 0-100
  bool battCharging = false; // 推断值（充电芯片状态没接到 GPIO）
  bool powerBattery = false; // true = 电池档（低频刷新），false = 市电档（默认）

  int page = 0;  // 0/1 监视页，2 伴侣页，3 对话列表
  bool screenOn = true;
  uint8_t brightness = 150;
  uint32_t lastSampleMs = 0;
};

extern DashState g;

inline void pushHistory(int64_t down, int64_t up) {
  g.histDown[g.histHead] = down;
  g.histUp[g.histHead] = up;
  g.histHead = (g.histHead + 1) % 60;
  if (g.histLen < 60) g.histLen++;
}

// 去掉 emoji（4 字节 UTF-8）与非法字节，只保留 ASCII 和 2/3 字节 UTF-8。
inline void sanitizeUtf8(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(src ? src : "");
  while (*p && o + 1 < cap) {
    const unsigned char c = *p;
    if (c < 0x80) {
      dst[o++] = static_cast<char>(c);
      p++;
    } else if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      dst[o++] = static_cast<char>(p[0]);
      dst[o++] = static_cast<char>(p[1]);
      p += 2;
    } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      dst[o++] = static_cast<char>(p[0]);
      dst[o++] = static_cast<char>(p[1]);
      dst[o++] = static_cast<char>(p[2]);
      p += 3;
    } else {
      p++;  // 4 字节 emoji 或非法字节，跳过。
    }
  }
  dst[o] = '\0';
}

// U8g2 的 wqy14 字库里没有这些常见标点（会整个消失），统一换成空格并压缩空白。
inline void normalizeGlyphs(char* s) {
  unsigned char* r = reinterpret_cast<unsigned char*>(s);
  unsigned char* w = r;
  bool lastSpace = true;  // 去掉开头空白
  while (*r) {
    int len = 1;
    bool space = false;
    if (r[0] == ' ') {
      space = true;
    } else if (r[0] == 0xC2 && r[1] == 0xB7) {                       // · U+00B7
      len = 2;
      space = true;
    } else if (r[0] == 0xE2 && r[1] == 0x80 &&
               (r[2] == 0xA2 || r[2] == 0x93 || r[2] == 0x94)) {     // • – —
      len = 3;
      space = true;
    } else if (r[0] == 0xE3 && r[1] == 0x83 && r[2] == 0xBB) {       // ・ U+30FB
      len = 3;
      space = true;
    } else if (r[0] >= 0xF0) {
      len = 4;
    } else if (r[0] >= 0xE0) {
      len = 3;
    } else if (r[0] >= 0xC0) {
      len = 2;
    }
    if (space) {
      if (!lastSpace) *w++ = ' ';
      lastSpace = true;
    } else {
      for (int i = 0; i < len && r[i]; i++) *w++ = r[i];
      lastSpace = false;
    }
    for (int i = 0; i < len && *r; i++) r++;
  }
  if (w > reinterpret_cast<unsigned char*>(s) && w[-1] == ' ') w--;  // 去掉结尾空白
  *w = 0;
}
