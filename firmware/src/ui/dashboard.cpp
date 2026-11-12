#include "dashboard.h"

#include <stdio.h>
#include <string.h>

#include "ST7305_U8g2.h"
#include "frame_diff.h"
#include "page_logic.h"
#include "pet_sprites.h"
#include "../board.h"
#include "../net/codex_client.h"
#include "../net/power.h"
#include "../state.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy firmware/include/config.example.h to firmware/include/config.h before building."
#endif

namespace {

// ---- 字体（U8g2 内置，无需外部字体文件）----
const uint8_t* kFontMed = u8g2_font_helvB14_tf;   // 数值（速度、百分比）
const uint8_t* kFontSmall = u8g2_font_6x13_tf;    // IP、延迟、小号 ASCII
const uint8_t* kFontZh = u8g2_font_wqy14_t_gb2312; // 中文 14px（注意：没有「·」等标点，见 normalizeGlyphs）

const char* kLatNames[kLatencySites] = {"Baidu", "CF", "OpenAI", "Github", "YouTube"};

ST7305_U8g2 lcd(PIN_RLCD_SCK, PIN_RLCD_MOSI, PIN_RLCD_DC, PIN_RLCD_CS, PIN_RLCD_RST);
U8G2* u8g2 = nullptr;

uint32_t lastPaintMs = 0;
int lastLink = -1;
int lastPage = -1;

// ---- 布局（横屏 400x300，与预览 v3 一致）----
constexpr int kMargin = 12;       // 统一左边距
constexpr int kDivX = 200;        // 竖分隔线

// 速度区。
constexpr int kWanSpeedY = 26;    // 第 1 行：路由器 WAN 总流量
constexpr int kProxySpeedY = 50;  // 第 2 行：Clash 代理流量
constexpr int kSpeedDivY = 62;
constexpr int kSpeedLabelX = kMargin;
constexpr int kSpeedDownX = 56;
constexpr int kSpeedUpX = 216;

// 网络信息区（左右两列：国内左、全球右，中间竖线）。
constexpr int kNetChinaX = kMargin;
constexpr int kNetGlobalX = 212;
constexpr int kNetColW = 176;
constexpr int kNetLabelY = 88;
constexpr int kNetIpY = 106;
constexpr int kNetLocY = 122;
constexpr int kNetDivY = 138;
constexpr int kNetVLineY = 66;
constexpr int kNetVLineH = 70;

// 下半部：左列 = 延迟（上）+ Grok 用量（下），右列 = Codex。
constexpr int kLatFirstY = 157;   // 延迟第一行基线
constexpr int kLatRowH = 18;
constexpr int kLatBoxX = 80;      // 状态方块 12x12
constexpr int kLatValRight = 186; // 数值右对齐
constexpr int kLowerVLineY = 141;
constexpr int kLowerVLineH = 159;

// Grok 用量块（左下）。
constexpr int kGrokDivY = 237;
constexpr int kGrokX0 = kMargin;
constexpr int kGrokX1 = 188;
constexpr int kGrokTitleY = 258;
constexpr int kGrokPctY = 259;
constexpr int kGrokBarY = 266;
constexpr int kGrokSubY = 289;

// Codex 面板（右下）：文字区 x 214..386，虚线框 206..394。
constexpr int kCodexX0 = 214;
constexpr int kCodexX1 = 386;
constexpr int kCodexIconY = 149;
constexpr int kCodexLabelY = 161;
constexpr int kCodexWindowY = 181;
constexpr int kCodexWeeklyY = 205;
constexpr int kCodexWeeklyBarY = 210;
constexpr int kCodexCtxY = 237;
constexpr int kCodexCtxBarY = 242;
constexpr int kCodexCtxTextY = 267;
constexpr int kCodexFooterY = 287;

constexpr float kHighUsage = 85.0f;  // 用量 >= 此值：反白标题 + 粗边框进度条

// ---- 按钮 ----
constexpr int PIN_BTN_PAGE = PIN_BTN_KEY;   // KEY：短按伴侣页/列表，长按息屏/亮屏
constexpr int PIN_BTN_POWER = PIN_BTN_BOOT; // BOOT：短按回到/切换监视页（不再息屏）

constexpr uint32_t kDebounceMs = 30;    // 消抖窗口
constexpr uint32_t kLongPressMs = 700;  // 长按阈值

struct Button {
  int pin;
  bool state;          // 消抖后的稳定状态：false=未按，true=按下
  bool lastRaw;        // 上一次原始读取
  uint32_t rawChangedAt; // 原始信号最近一次变化时间
  uint32_t downAt;     // 稳定按下的起始时间
  bool longFired;      // 长按是否已触发（防止松开时再触发短按）
};

Button btnPage = {PIN_BTN_PAGE, false, false, 0, 0, false};
Button btnPower = {PIN_BTN_POWER, false, false, 0, 0, false};

void readButton(Button& b, void (*onShort)(), void (*onLong)()) {
  const bool raw = digitalRead(b.pin) == LOW;
  const uint32_t now = millis();

  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.rawChangedAt = now;
  }

  // 原始信号稳定超过消抖窗口后，才更新稳定状态。
  if (raw != b.state && now - b.rawChangedAt >= kDebounceMs) {
    b.state = raw;
    if (raw) {
      b.downAt = now;
      b.longFired = false;
    } else if (!b.longFired) {
      if (onShort) onShort();
    }
  }

  // 长按：持续按住超过阈值立即触发。
  if (b.state && !b.longFired && onLong && now - b.downAt >= kLongPressMs) {
    b.longFired = true;
    onLong();
  }
}

void onPageShort() {
  g.page = onKeyShort(g.page);
  lastPage = -1;
}

bool hasPrevFrame = false;  // 上一帧拷贝是否有效（见 presentFrame）

void setScreenOn(bool on) {
  g.screenOn = on;
  u8g2->setPowerSave(on ? 0 : 1);
  if (on) hasPrevFrame = false;  // 重新点亮后整屏重发一次，保证画面完整
}

void onPageLong() {
  setScreenOn(!g.screenOn);
}

void onPowerShort() {
  g.page = onBootShort(g.page);
  lastPage = -1;
}

// ---- 格式化 ----
void splitSpeed(int64_t v, char* num, size_t numCap, char* unit, size_t unitCap) {
  const char* units[] = {"B/s", "kB/s", "MB/s", "GB/s", "TB/s"};
  double d = static_cast<double>(v);
  int u = 0;
  while (d >= 1000.0 && u < 4) {
    d /= 1000.0;
    u++;
  }
  if (d < 10.0) snprintf(num, numCap, "%.2f", d);
  else if (d < 100.0) snprintf(num, numCap, "%.1f", d);
  else snprintf(num, numCap, "%.0f", d);
  snprintf(unit, unitCap, "%s", units[u]);
}

// 字节数自适应单位（1024 进制）。
void formatBytes(char* out, size_t n, int64_t v) {
  const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double d = static_cast<double>(v);
  int u = 0;
  while (d >= 1024.0 && u < 4) {
    d /= 1024.0;
    u++;
  }
  if (u == 0) snprintf(out, n, "%.0f B", d);
  else if (d < 10.0) snprintf(out, n, "%.1f %s", d, units[u]);
  else snprintf(out, n, "%.0f %s", d, units[u]);
}

// ---- 基础绘制 ----
void setFont(const uint8_t* f) { u8g2->setFont(f); }

// 居中绘制（ASCII）。cx 为水平中心，y 为基线。
void drawStrCentered(int cx, int y, const char* s) {
  int w = u8g2->getStrWidth(s);
  u8g2->drawStr(cx - w / 2, y, s);
}

// UTF-8 截断到 maxW 像素并绘制（用于可能含中文的文本，超长加省略号）。
void drawUTF8Fit(int x, int y, const char* s, int maxW) {
  if (!s || !s[0]) return;
  int w = u8g2->getUTF8Width(s);
  if (w <= maxW) {
    u8g2->drawUTF8(x, y, s);
    return;
  }
  static const char kEll[] = {static_cast<char>(0xE2), static_cast<char>(0x80),
                              static_cast<char>(0xA6), 0}; // "…"
  int ellW = u8g2->getUTF8Width(kEll);
  int limit = maxW - ellW;
  char buf[160];
  size_t used = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  int acc = 0;
  while (*p && used + 4 < sizeof(buf)) {
    const unsigned char* start = p;
    int len;
    if (*p < 0x80) len = 1;
    else if ((*p & 0xE0) == 0xC0) len = 2;
    else if ((*p & 0xF0) == 0xE0) len = 3;
    else { p++; continue; }
    char tmp[8];
    memcpy(tmp, start, len);
    tmp[len] = 0;
    int cw = u8g2->getUTF8Width(tmp);
    if (acc + cw > limit) break;
    memcpy(buf + used, start, len);
    used += len;
    acc += cw;
    p += len;
  }
  memcpy(buf + used, kEll, 3);
  used += 3;
  buf[used] = 0;
  u8g2->drawUTF8(x, y, buf);
}

void drawArrow(int cx, int cy, int half, bool up) {
  if (up) {
    u8g2->drawTriangle(cx, cy - half, cx - half, cy + half, cx + half, cy + half);
  } else {
    u8g2->drawTriangle(cx, cy + half, cx - half, cy - half, cx + half, cy - half);
  }
}

// 速度单元：箭头 + 数字 + 单位。
void drawSpeedUnit(int x, int y, bool up, int64_t bps) {
  drawArrow(x + 6, y - 7, 5, up);
  char num[12];
  char unit[8];
  splitSpeed(bps, num, sizeof(num), unit, sizeof(unit));

  setFont(kFontMed);
  const int nx = x + 14;
  u8g2->drawStr(nx, y, num);
  const int numW = u8g2->getStrWidth(num);

  setFont(kFontSmall);
  const int ux = nx + numW + 4; // 单位在数字右侧，留 4px 间距
  u8g2->drawStr(ux, y - 1, unit);
}

// 一行速度：中文标签 + 下载列 + 上传列。
void drawSpeedRow(int y, const char* label, int64_t downBps, int64_t upBps, const char* unavailable = nullptr) {
  setFont(kFontZh);
  u8g2->drawUTF8(kSpeedLabelX, y, label);
  if (unavailable && unavailable[0]) {
    u8g2->drawUTF8(kSpeedDownX, y, unavailable);
    return;
  }
  drawSpeedUnit(kSpeedDownX, y, false, downBps);
  drawSpeedUnit(kSpeedUpX, y, true, upBps);
}

// 网络信息列：标签（中文）+ IP（6x13）+ 归属地，垂直三行。
void drawNetCol(int x, const char* label, const char* ip, const char* loc) {
  setFont(kFontZh);
  u8g2->drawUTF8(x, kNetLabelY, label);
  if (ip && ip[0]) {
    setFont(kFontSmall);
    u8g2->drawStr(x, kNetIpY, ip);
  }
  setFont(kFontZh);
  if (loc && loc[0]) {
    drawUTF8Fit(x, kNetLocY, loc, kNetColW);
  } else if (g.link == LINK_WIFI) {
    u8g2->drawUTF8(x, kNetLocY, "未连接");
  } else {
    u8g2->drawUTF8(x, kNetLocY, "获取中...");
  }
}

// 第二页条目：标题 + 主机名 + 累计流量。
void drawTopHostBlock(int y, const char* title, const TopHost& t) {
  setFont(kFontZh);
  u8g2->drawUTF8(12, y, title);
  y += 26;
  if (t.host[0]) {
    setFont(kFontSmall);
    drawUTF8Fit(12, y, t.host, kWidth - 24);
    y += 20;
    char buf[32];
    formatBytes(buf, sizeof(buf), t.bytes);
    setFont(kFontSmall);
    u8g2->drawStr(12, y, buf);
  } else if (g.link != LINK_OK) {
    setFont(kFontZh);
    u8g2->drawUTF8(12, y, (g.link == LINK_WIFI) ? "Wi-Fi 未连接" : "代理未开启");
  } else {
    setFont(kFontZh);
    u8g2->drawUTF8(12, y, "暂无");
  }
}

// ---- 页面 0 的小工具 ----
void drawRight(int xr, int y, const char* s) {
  u8g2->drawUTF8(xr - u8g2->getUTF8Width(s), y, s);
}

void drawDashedFrame(int x, int y, int w, int h) {
  for (int i = 0; i < w; i += 6) {
    const int l = (w - i < 3) ? (w - i) : 3;
    u8g2->drawHLine(x + i, y, l);
    u8g2->drawHLine(x + i, y + h - 1, l);
  }
  for (int j = 0; j < h; j += 6) {
    const int l = (h - j < 3) ? (h - j) : 3;
    u8g2->drawVLine(x, y + j, l);
    u8g2->drawVLine(x + w - 1, y + j, l);
  }
}

// 进度条（高 8px）：<55% 细填充，55~85% 粗填充，>=85% 粗填充 + 加粗边框。pct<0 只画空框。
void drawBar(int x, int y, int w, float pct) {
  const int h = 8;
  u8g2->drawFrame(x, y, w, h);
  if (pct < 0.0f) return;
  if (pct > 100.0f) pct = 100.0f;
  const int fw = static_cast<int>((w - 4) * pct / 100.0f + 0.5f);
  if (pct >= kHighUsage) {
    u8g2->drawFrame(x + 1, y + 1, w - 2, h - 2);
    u8g2->drawBox(x + 2, y + 2, fw, h - 4);
  } else if (pct >= 55.0f) {
    u8g2->drawBox(x + 2, y + 2, fw, h - 4);
  } else {
    u8g2->drawBox(x + 2, y + 3, fw, h - 6);
  }
}

void formatPercent(char* out, size_t n, float pct) {
  if (pct < 0.0f) snprintf(out, n, "--");
  else snprintf(out, n, "%.0f%%", pct);
}

void formatElapsed(char* out, size_t n, int seconds) {
  if (seconds < 0) seconds = 0;
  const int hours = seconds / 3600;
  const int minutes = (seconds % 3600) / 60;
  const int secs = seconds % 60;
  if (hours > 0) snprintf(out, n, "%d小时%d分", hours, minutes);
  else if (minutes > 0) snprintf(out, n, "%d分%02d秒", minutes, secs);
  else snprintf(out, n, "%d秒", secs);
}

void formatAgo(char* out, size_t n, uint32_t seconds) {
  if (seconds < 45) snprintf(out, n, "刚刚");
  else if (seconds < 3600) snprintf(out, n, "%u 分钟前", static_cast<unsigned>(seconds / 60));
  else if (seconds < 86400) snprintf(out, n, "%u 小时前", static_cast<unsigned>(seconds / 3600));
  else snprintf(out, n, "%u 天前", static_cast<unsigned>(seconds / 86400));
}

// ---- 延迟（左列上部，与 Mac 是否在线无关）----
void paintLatency() {
  for (int i = 0; i < kLatencySites; i++) {
    const int y = kLatFirstY + i * kLatRowH;
    setFont(kFontSmall);
    u8g2->drawStr(kMargin, y, kLatNames[i]);

    const int16_t ms = g.latency[i];
    if (ms < 0 && g.link != LINK_OK) {  // 代理或 Wi-Fi 不可用：不把失败画成「超时」
      drawRight(kLatValRight, y, "--");
      continue;
    }
    if (ms == -2) {  // 未测
      drawRight(kLatValRight, y, "--");
      continue;
    }
    const int boxY = y - 11;
    if (ms == -1) {  // 超时：× + 文字
      u8g2->drawLine(kLatBoxX, boxY, kLatBoxX + 11, boxY + 11);
      u8g2->drawLine(kLatBoxX + 11, boxY, kLatBoxX, boxY + 11);
      setFont(kFontZh);
      drawRight(kLatValRight, y, "超时");
      continue;
    }
    if (ms < 300) {
      u8g2->drawBox(kLatBoxX, boxY, 12, 12);      // 实心 = 快
    } else if (ms <= 800) {
      u8g2->drawFrame(kLatBoxX, boxY, 12, 12);    // 空心 + 横线 = 中
      u8g2->drawHLine(kLatBoxX + 1, boxY + 6, 10);
    } else {
      u8g2->drawFrame(kLatBoxX, boxY, 12, 12);    // 空心 = 慢
    }
    char val[16];
    snprintf(val, sizeof(val), "%d ms", ms);
    setFont(kFontSmall);
    drawRight(kLatValRight, y, val);
  }
}

// ---- Grok Bot 用量（左列下部）----
// 约 N 小时/天后重置；缓存已过期（重置时间已过）则提示待刷新。
void grokResetText(char* out, size_t n) {
  const CodexState& c = g.codex;
  const int64_t now = codexUnixNow();
  if (c.grokResetUnix <= 0 || now <= 0 || c.grokResetUnix <= now) {
    snprintf(out, n, "用量待刷新");
    return;
  }
  const int64_t remain = c.grokResetUnix - now;
  const int hours = static_cast<int>((remain + 3599) / 3600);
  if (hours < 48) snprintf(out, n, "约 %d 小时后重置", hours);
  else snprintf(out, n, "约 %d 天后重置", static_cast<int>((remain + 43200) / 86400));
}

void paintGrok() {
  const CodexState& c = g.codex;
  const bool macUp = codexLinkNow() == CODEX_OK;
  u8g2->drawHLine(0, kGrokDivY, kDivX);

  float pct = -1.0f;
  char sub[40];
  if (!macUp) {
    // Mac 不在线：虚线框 + 未连接 Mac + --
    drawDashedFrame(4, 241, 188, 56);
    snprintf(sub, sizeof(sub), "未连接 Mac");
  } else if (c.grokState == GROK_OK) {
    pct = c.grokPercent;
    grokResetText(sub, sizeof(sub));
  } else {
    // 应用未运行/读不到：保留最近一次缓存的用量。
    pct = c.grokPercent;
    snprintf(sub, sizeof(sub), "%s", c.grokState == GROK_CLOSED ? "应用未运行" : "应用未连接");
  }

  const bool high = pct >= kHighUsage;
  if (high) {
    u8g2->drawBox(8, 243, 180, 20);
    u8g2->setDrawColor(0);
  }
  setFont(kFontZh);
  u8g2->drawUTF8(kGrokX0, kGrokTitleY, "Grok Bot");
  char pctText[12];
  formatPercent(pctText, sizeof(pctText), pct);
  setFont(kFontMed);
  drawRight(kGrokX1, kGrokPctY, pctText);
  u8g2->setDrawColor(1);

  drawBar(kGrokX0, kGrokBarY, kGrokX1 - kGrokX0, pct);
  setFont(kFontZh);
  u8g2->drawUTF8(kGrokX0, kGrokSubY, sub);
}

// ---- Codex 面板（右列）----
enum PanelKind : uint8_t {
  PK_RUNNING,
  PK_COMPLETE,
  PK_INTERRUPTED,
  PK_IDLE,
  PK_QUOTA,
  PK_STALE,
  PK_OFFLINE,
};

// 12x12 状态图标，(x,y) 为左上角。
void drawStatusIcon(PanelKind kind, int x, int y) {
  const int cx = x + 6;
  const int cy = y + 6;
  switch (kind) {
    case PK_RUNNING:
      u8g2->drawDisc(cx, cy, 6);
      break;
    case PK_IDLE:
      u8g2->drawCircle(cx, cy, 6);
      break;
    case PK_COMPLETE:
      u8g2->drawFrame(x, y, 12, 12);
      u8g2->drawLine(x + 3, y + 6, x + 5, y + 8);
      u8g2->drawLine(x + 5, y + 8, x + 9, y + 3);
      u8g2->drawLine(x + 3, y + 7, x + 5, y + 9);
      u8g2->drawLine(x + 5, y + 9, x + 9, y + 4);
      break;
    case PK_INTERRUPTED:
    case PK_STALE:
    case PK_OFFLINE:
      u8g2->drawCircle(cx, cy, 6);
      u8g2->drawLine(x + 3, y + 3, x + 9, y + 9);
      u8g2->drawLine(x + 9, y + 3, x + 3, y + 9);
      break;
    case PK_QUOTA:
      u8g2->drawVLine(cx, y + 1, 7);
      u8g2->drawVLine(cx + 1, y + 1, 7);
      u8g2->drawBox(cx, y + 10, 2, 2);
      break;
  }
}

int shownElapsed() {
  const CodexState& c = g.codex;
  if (strcmp(c.status, "running") == 0 && c.turnStartedAt > 0) {
    const int64_t now = codexUnixNow();
    if (now >= c.turnStartedAt) return static_cast<int>(now - c.turnStartedAt);
  }
  return c.elapsedSeconds < 0 ? 0 : c.elapsedSeconds;
}

void paintCodex() {
  const CodexState& c = g.codex;
  const CodexLink link = codexLinkNow();
  u8g2->drawVLine(kDivX, kLowerVLineY, kLowerVLineH);

  PanelKind kind = PK_OFFLINE;
  char label[56];
  char windowText[56] = "";
  char elapsed[24] = "";
  char footer[64] = "";
  float weekly = -1.0f;
  float context = -1.0f;
  const char* ctxText = "--";

  if (link == CODEX_NEVER) {
    kind = PK_OFFLINE;
    if (c.authFailed) {
      snprintf(label, sizeof(label), "令牌错误");
      snprintf(windowText, sizeof(windowText), "检查 CODEX_TOKEN");
      snprintf(footer, sizeof(footer), "Mac 拒绝了令牌");
    } else {
      snprintf(label, sizeof(label), "未连接 Mac");
      snprintf(windowText, sizeof(windowText), "等待 codex-status");
      snprintf(footer, sizeof(footer), "请检查 Mac 与服务");
    }
  } else {
    snprintf(windowText, sizeof(windowText), "%s", c.window);
    weekly = c.weekly;
    context = c.context;
    ctxText = c.ctxText[0] ? c.ctxText : "--";
    if (link == CODEX_STALE) {
      kind = PK_STALE;
      snprintf(label, sizeof(label), "连接中断");
      char ago[24];
      formatAgo(ago, sizeof(ago), (millis() - c.lastOkMs) / 1000UL);
      snprintf(footer, sizeof(footer), "上次更新 %s", ago);
    } else {
      snprintf(footer, sizeof(footer), "%s", c.footer);
      snprintf(label, sizeof(label), "%s", c.label[0] ? c.label : "等待中");
      if (!c.hasSession) {
        kind = PK_IDLE;
        snprintf(label, sizeof(label), "暂无会话");
        snprintf(windowText, sizeof(windowText), "等待 Codex 会话");
      } else if (c.quota) {
        kind = PK_QUOTA;  // 标签沿用 sidecar 的（「额度将尽」或「额度将尽 跑命令」）
        if (!c.label[0]) snprintf(label, sizeof(label), "额度将尽");
      } else if (strcmp(c.status, "running") == 0) {
        kind = PK_RUNNING;
      } else if (strcmp(c.status, "complete") == 0) {
        kind = PK_COMPLETE;
      } else if (strcmp(c.status, "interrupted") == 0) {
        kind = PK_INTERRUPTED;
      } else {
        kind = PK_IDLE;
      }
      if (c.hasSession && (strcmp(c.status, "running") == 0 || strcmp(c.status, "complete") == 0 ||
                           strcmp(c.status, "interrupted") == 0)) {
        formatElapsed(elapsed, sizeof(elapsed), shownElapsed());
      }
    }
  }

  if (kind == PK_STALE || kind == PK_OFFLINE) drawDashedFrame(206, 143, 189, 154);

  // 状态行（额度将尽时整行反白）。
  const bool rev = kind == PK_QUOTA;
  if (rev) {
    u8g2->drawBox(212, 146, 176, 20);
    u8g2->setDrawColor(0);
  }
  drawStatusIcon(kind, kCodexX0, kCodexIconY);
  setFont(kFontZh);
  drawUTF8Fit(kCodexX0 + 18, kCodexLabelY, label, kCodexX1 - kCodexX0 - 18);
  u8g2->setDrawColor(1);

  setFont(kFontZh);
  if (windowText[0]) drawUTF8Fit(kCodexX0, kCodexWindowY, windowText, kCodexX1 - kCodexX0);

  char pctText[12];
  u8g2->drawUTF8(kCodexX0, kCodexWeeklyY, "本周");
  formatPercent(pctText, sizeof(pctText), weekly);
  setFont(kFontMed);
  drawRight(kCodexX1, kCodexWeeklyY, pctText);
  drawBar(kCodexX0, kCodexWeeklyBarY, kCodexX1 - kCodexX0, weekly);

  setFont(kFontZh);
  u8g2->drawUTF8(kCodexX0, kCodexCtxY, "上下文");
  formatPercent(pctText, sizeof(pctText), context);
  setFont(kFontMed);
  drawRight(kCodexX1, kCodexCtxY, pctText);
  drawBar(kCodexX0, kCodexCtxBarY, kCodexX1 - kCodexX0, context);

  setFont(kFontSmall);
  u8g2->drawStr(kCodexX0, kCodexCtxTextY, ctxText);
  if (elapsed[0]) {
    setFont(kFontZh);
    drawRight(kCodexX1, kCodexCtxTextY, elapsed);
  }
  setFont(kFontZh);
  drawUTF8Fit(kCodexX0, kCodexFooterY, footer, kCodexX1 - kCodexX0);
}

// ---- 电池图标（首页右上角，与「外网」行基线对齐）----
constexpr int kBattX = 358;        // 机身左上角，26x13，右边缘 384，正极凸起到 386
constexpr int kBattY = 13;
constexpr int kBattW = 26;
constexpr int kBattH = 13;
constexpr int kBattTextRight = 354;  // 百分比右对齐位置（6x13）
constexpr int kBattTextY = kBattY + 11;
constexpr int kBattLow = 15;         // ≤15% 百分比反白

// 7x11 闪电（充电推断），相对左上角。
constexpr uint8_t kBolt[][2] = {
    {4, 0}, {3, 1}, {3, 2}, {2, 3}, {2, 4}, {1, 5}, {0, 6}, {1, 6}, {2, 6}, {3, 6}, {4, 6}, {3, 7}, {3, 8},
    {2, 9}, {2, 10}, {4, 1}, {4, 2}, {3, 3}, {3, 4}, {2, 5}, {5, 6}, {6, 6}, {4, 7}, {4, 8}, {3, 9},
};

void paintBattery() {
  if (!g.battValid || g.battPercent < 0) return;  // 读不到合理电压（没接电池）：不画
  const int pct = g.battPercent > 100 ? 100 : g.battPercent;
  u8g2->drawFrame(kBattX, kBattY, kBattW, kBattH);
  u8g2->drawBox(kBattX + kBattW, kBattY + 4, 2, 5);  // 正极凸起
  const int fw = (kBattW - 4) * pct / 100;
  if (fw > 0) u8g2->drawBox(kBattX + 2, kBattY + 2, fw, kBattH - 4);

  char txt[8];
  snprintf(txt, sizeof(txt), "%d%%", pct);
  setFont(kFontSmall);
  const int tw = u8g2->getStrWidth(txt);
  const bool low = pct <= kBattLow;
  if (low) {  // 低电量：百分比反白成块
    u8g2->drawBox(kBattTextRight - tw - 3, kBattY - 1, tw + 6, kBattH + 2);
    u8g2->setDrawColor(0);
    u8g2->drawStr(kBattTextRight - tw, kBattTextY, txt);
    u8g2->setDrawColor(1);
  } else {
    u8g2->drawStr(kBattTextRight - tw, kBattTextY, txt);
  }
  if (g.battCharging) {  // 闪电放在百分比左侧
    const int bx = kBattTextRight - tw - 12 - (low ? 3 : 0);
    for (const auto& p : kBolt) u8g2->drawPixel(bx + p[0], kBattY + 1 + p[1]);
  }
}

// ---- 页面 0：速度 + 网络信息 + 延迟/Grok + Codex ----
void paintPage0() {
  // 速度两行：路由器 WAN 总流量 + Clash 代理流量。
  // 代理没开或 Wi-Fi 断了时仍画整页，只把受影响的行标成不可用，不再整屏黑掉。
  const char* wanMsg = (g.link == LINK_WIFI) ? "未连接" : nullptr;
  const char* proxyMsg = nullptr;
  if (g.link == LINK_WIFI) proxyMsg = "未连接";
  else if (g.link == LINK_401) proxyMsg = "密码错误";
  else if (g.link == LINK_DOWN) proxyMsg = "不可用";
  drawSpeedRow(kWanSpeedY, "外网", g.wanDownSpeed, g.wanUpSpeed, wanMsg);
  drawSpeedRow(kProxySpeedY, "代理", g.downSpeed, g.upSpeed, proxyMsg);
  paintBattery();
  u8g2->drawHLine(0, kSpeedDivY, kWidth);

  // 网络信息：左右两列 + 竖线。
  drawNetCol(kNetChinaX, "国内", g.chinaIp, g.chinaLoc);
  drawNetCol(kNetGlobalX, "全球", g.globalIp, g.globalLoc);
  u8g2->drawVLine(kDivX, kNetVLineY, kNetVLineH);
  u8g2->drawHLine(0, kNetDivY, kWidth);

  // 下半部：左列延迟 + Grok，右列 Codex。延迟与 Mac 在线与否无关。
  paintLatency();
  paintGrok();
  paintCodex();
}

// ---- 页面 1：当前节点 + 下载/上传最多 ----
void paintPage1() {
  int y = 44;
  setFont(kFontZh);
  u8g2->drawUTF8(12, y, "当前节点");
  y += 30;
  if (g.link == LINK_WIFI) {
    setFont(kFontZh);
    u8g2->drawUTF8(12, y, "Wi-Fi 未连接");
  } else if (g.link == LINK_401) {
    setFont(kFontZh);
    u8g2->drawUTF8(12, y, "代理密码错误");
  } else if (g.link == LINK_DOWN) {
    setFont(kFontZh);
    u8g2->drawUTF8(12, y, "代理未开启");
  } else if (g.currentNode[0]) {
    setFont(kFontZh);
    drawUTF8Fit(12, y, g.currentNode, kWidth - 24);
  } else {
    setFont(kFontZh);
    u8g2->drawUTF8(12, y, "获取中...");
  }
  y += 50;

  drawTopHostBlock(y, "下载最多", g.topDown);
  y += 76;
  drawTopHostBlock(y, "上传最多", g.topUp);
}

void paintStatus() {
  const char* title = "Wi-Fi 未连接";
  const char* sub = "正在重连...";
  if (g.link == LINK_401) {
    title = "密码错误 (401)";
    sub = "请检查面板密码";
  } else if (g.link == LINK_DOWN) {
    title = "API 不可用";
    sub = "正在重试...";
  }
  setFont(kFontZh);
  drawStrCentered(kWidth / 2, 120, title);
  setFont(kFontSmall);
  drawStrCentered(kWidth / 2, 150, sub);
}

// ---- 伴侣页 / 对话列表（page 2 / 3，布局与批准的 preview 一致）----
constexpr int kPetW = 128;
constexpr int kPetH = 139;
constexpr int kPetFooterY = 272;
constexpr int kPetBattNubX = 388;
constexpr int kPetBattY = 277;

void chamferFrame(int x, int y, int w, int h, int cut) {
  u8g2->drawHLine(x + cut, y, w - 2 * cut);
  u8g2->drawHLine(x + cut, y + h - 1, w - 2 * cut);
  u8g2->drawVLine(x, y + cut, h - 2 * cut);
  u8g2->drawVLine(x + w - 1, y + cut, h - 2 * cut);
  for (int k = 0; k < cut; k++) {
    u8g2->drawPixel(x + cut - 1 - k, y + k);
    u8g2->drawPixel(x + w - cut + k, y + k);
    u8g2->drawPixel(x + cut - 1 - k, y + h - 1 - k);
    u8g2->drawPixel(x + w - cut + k, y + h - 1 - k);
  }
}

int iround(int num, int den) { return (num + den / 2) / den; }

// 画气泡，返回尾巴底部的 y。
int paintBubble(int cx, int top, const char* title, const char* label, bool invertLabel) {
  setFont(kFontZh);
  const int padX = 10, padY = 6, gap = 4, titleH = 16, stateH = 16, maxW = 340;
  const int titleW = title && title[0] ? u8g2->getUTF8Width(title) : 0;
  const int labelW = label && label[0] ? u8g2->getUTF8Width(label) : 0;
  int innerW = titleW > labelW ? titleW : labelW;
  const int cap = maxW - 2 * padX;
  if (innerW > cap) innerW = cap;
  if (innerW < 8) innerW = 8;
  const int bw = innerW + 2 * padX;
  const int bh = padY + titleH + gap + stateH + padY;
  int x = cx - bw / 2;
  if (x < 6) x = 6;
  if (x + bw > kWidth - 6) x = kWidth - 6 - bw;
  const int y = top;
  chamferFrame(x, y, bw, bh, 3);

  const int tailH = 6, half = 5;
  int tx = cx;
  if (tx < x + 12) tx = x + 12;
  if (tx > x + bw - 12) tx = x + bw - 12;
  u8g2->setDrawColor(0);
  u8g2->drawHLine(tx - half, y + bh - 1, half * 2 + 1);
  u8g2->setDrawColor(1);
  for (int k = 0; k <= tailH; k++) {
    const int inset = iround(half * k, tailH);
    const int yy = y + bh - 1 + k;
    u8g2->drawPixel(tx - half + inset, yy);
    u8g2->drawPixel(tx + half - inset, yy);
  }

  setFont(kFontZh);
  if (title && title[0]) drawUTF8Fit(x + padX, y + padY + 13, title, innerW);
  const int sy = y + padY + titleH + gap + 13;
  if (label && label[0]) {
    if (invertLabel) {
      const int sw = u8g2->getUTF8Width(label);
      u8g2->drawBox(x + padX - 2, sy - 13, sw + 4, 16);
      u8g2->setDrawColor(0);
      u8g2->drawUTF8(x + padX, sy, label);
      u8g2->setDrawColor(1);
    } else {
      u8g2->drawUTF8(x + padX, sy, label);
    }
  }
  return y + bh - 1 + tailH;
}

// 画电池使正极凸起右缘落在 nubX。返回百分比（或闪电）左缘，给「本周」让位。
int paintBatteryAt(int nubX, int y) {
  if (!g.battValid || g.battPercent < 0) return nubX;
  const int pct = g.battPercent > 100 ? 100 : g.battPercent;
  const int bx = nubX - 2 - kBattW;
  const int by = y;
  u8g2->drawFrame(bx, by, kBattW, kBattH);
  u8g2->drawBox(bx + kBattW, by + 4, 2, 5);
  const int fw = (kBattW - 4) * pct / 100;
  if (fw > 0) u8g2->drawBox(bx + 2, by + 2, fw, kBattH - 4);
  char txt[8];
  snprintf(txt, sizeof(txt), "%d%%", pct);
  setFont(kFontSmall);
  const int tw = u8g2->getStrWidth(txt);
  const int tx = bx - 4 - tw;
  const bool low = pct <= kBattLow;
  if (low) {
    u8g2->drawBox(tx - 3, by - 1, tw + 6, kBattH + 2);
    u8g2->setDrawColor(0);
    u8g2->drawStr(tx, by + 11, txt);
    u8g2->setDrawColor(1);
  } else {
    u8g2->drawStr(tx, by + 11, txt);
  }
  if (g.battCharging) {
    const int boltX = tx - 10 - (low ? 3 : 0);
    for (const auto& p : kBolt) u8g2->drawPixel(boltX + p[0], by + 1 + p[1]);
    return boltX;
  }
  return tx;
}

void paintPetFooter() {
  u8g2->drawHLine(0, kPetFooterY, kWidth);
  setFont(kFontZh);
  const CodexState& c = g.codex;
  char counts[64];
  snprintf(counts, sizeof(counts), "待回复 %d   出错 %d   已完成 %d   运行中 %d",
           c.countNeedsInput, c.countBlocked, c.countReady, c.countRunning);
  u8g2->drawUTF8(8, 291, counts);
  const int left = paintBatteryAt(kPetBattNubX, kPetBattY);
  setFont(kFontZh);
  char week[20];
  if (c.weekly < 0) snprintf(week, sizeof(week), "本周 --");
  else snprintf(week, sizeof(week), "本周 %d%%", static_cast<int>(c.weekly + 0.5f));
  const int ww = u8g2->getUTF8Width(week);
  u8g2->drawUTF8(left - 10 - ww, 291, week);
}

void petHeadline(char* title, size_t tcap, char* label, size_t lcap, const char** stateOut) {
  const CodexState& c = g.codex;
  const char* state = c.petState[0] ? c.petState : "idle";
  const CodexLink link = codexLinkNow();
  if (c.authFailed) {
    strlcpy(title, "令牌错误", tcap);
    strlcpy(label, "空闲", lcap);
    *stateOut = "idle";
    return;
  }
  if (link == CODEX_NEVER) {
    strlcpy(title, "等待连接", tcap);
    strlcpy(label, "空闲", lcap);
    *stateOut = "idle";
    return;
  }
  if (link == CODEX_STALE) {
    strlcpy(title, "连接中断", tcap);
    strlcpy(label, "空闲", lcap);
    *stateOut = "idle";
    return;
  }
  if (c.petTitle[0]) strlcpy(title, c.petTitle, tcap);
  else strlcpy(title, "未命名", tcap);
  if (c.petLabel[0]) strlcpy(label, c.petLabel, lcap);
  else strlcpy(label, petLabelFor(state), lcap);
  *stateOut = state;
}

void paintPetPage() {
  char title[104];
  char label[24];
  const char* state = "idle";
  petHeadline(title, sizeof(title), label, sizeof(label), &state);
  const bool invert = strcmp(state, "needs_input") == 0;
  const int petX = (kWidth - kPetW) / 2;
  const int tailBelow = paintBubble(petX + kPetW / 2, 8, title, label, invert);
  int petY = tailBelow + 4;
  if (petY < 70) petY = 70;
  if (petY + kPetH > 268) petY = 268 - kPetH;
  const bool alt = runningAltFrame(millis(), !g.powerBattery, state);
  const int frame = petFrameFor(state, alt);
  u8g2->drawBitmap(petX, petY, kPetSpriteRowBytes, kPetSpriteH, petSpriteBits(frame));
  paintPetFooter();
}

void iconNeeds(int x, int y) {
  u8g2->drawFrame(x, y, 13, 13);
  for (int dx = 4; dx <= 8; dx++) u8g2->drawPixel(x + dx, y + 2);
  u8g2->drawPixel(x + 3, y + 3);
  u8g2->drawPixel(x + 9, y + 3);
  u8g2->drawPixel(x + 9, y + 4);
  u8g2->drawPixel(x + 8, y + 5);
  u8g2->drawPixel(x + 7, y + 6);
  u8g2->drawPixel(x + 6, y + 7);
  u8g2->drawPixel(x + 6, y + 10);
}

void iconBlocked(int x, int y) {
  u8g2->drawFrame(x, y, 13, 13);
  for (int k = 0; k < 7; k++) {
    u8g2->drawPixel(x + 3 + k, y + 3 + k);
    u8g2->drawPixel(x + 9 - k, y + 3 + k);
  }
}

void iconReady(int x, int y) {
  u8g2->drawFrame(x, y, 13, 13);
  const int pts[][2] = {{3, 7}, {4, 8}, {5, 9}, {6, 8}, {7, 7}, {8, 6}, {9, 5}, {10, 4}};
  for (const auto& p : pts) {
    u8g2->drawPixel(x + p[0], y + p[1]);
    u8g2->drawPixel(x + p[0], y + p[1] - 1);
  }
}

void iconRunning(int x, int y) { u8g2->drawDisc(x + 6, y + 6, 5); }
void iconIdle(int x, int y) { u8g2->drawCircle(x + 6, y + 6, 5); }

void paintListIcon(const char* state, int x, int y) {
  if (state && strcmp(state, "needs_input") == 0) iconNeeds(x, y);
  else if (state && strcmp(state, "blocked") == 0) iconBlocked(x, y);
  else if (state && strcmp(state, "ready") == 0) iconReady(x, y);
  else if (state && strcmp(state, "running") == 0) iconRunning(x, y);
  else iconIdle(x, y);
}

void paintListPage() {
  setFont(kFontZh);
  u8g2->drawUTF8(8, 22, "Codex 对话");
  paintBatteryAt(kPetBattNubX, 10);
  u8g2->drawHLine(0, 30, kWidth);
  const CodexState& c = g.codex;
  const int timeReserve = 50;
  for (int i = 0; i < c.threadCount && i < 8; i++) {
    const CodexThread& row = c.threads[i];
    const int y = 48 + i * 28;
    paintListIcon(row.state, 10, y - 12);
    setFont(kFontZh);
    char age[16];
    formatPetAge(age, sizeof(age), row.ageS);
    const int tw = u8g2->getUTF8Width(age);
    u8g2->drawUTF8(kWidth - 8 - tw, y, age);
    if (row.title[0]) drawUTF8Fit(28, y, row.title, kWidth - 8 - timeReserve - 28 - 6);
  }
  u8g2->drawHLine(0, 270, kWidth);
  setFont(kFontZh);
  u8g2->drawUTF8(8, 291, "再按 KEY 返回");
}

// ---- 帧提交：只在内容变化时发送，且只发变化的图块区间 ----
// 以前每次重绘（市电每秒）都把整屏 15KB 重写一遍，即使画面没变；反射式 ST7305 屏上整屏写入可能肉眼可见闪烁。
// 现在保存上一帧的拷贝：完全相同就跳过；不同就只发变化的图块（u8g2 的 updateDisplayArea，驱动的
// DRAW_TILE 回调支持任意 x_pos/cnt）。首帧、屏幕重新点亮、区间过多时整屏发送。显示初始化只在开机做一次。
constexpr int kMaxRuns = 160;
uint8_t* prevFrame = nullptr;
framediff::Stats frameStats;
uint32_t lastFrameStatsMs = 0;

void presentFrame() {
  uint8_t* cur = u8g2->getBufferPtr();
  const int tileW = u8g2->getBufferTileWidth();
  const int tileH = u8g2->getBufferTileHeight();
  const size_t size = static_cast<size_t>(tileW) * tileH * framediff::kTileBytes;
  if (!prevFrame) prevFrame = static_cast<uint8_t*>(malloc(size));
  if (!prevFrame) {  // 内存不足：退回老办法
    u8g2->sendBuffer();
    return;
  }

  framediff::Run runs[kMaxRuns];
  int n = -1;
  if (hasPrevFrame) n = framediff::computeDirty(prevFrame, cur, tileW, tileH, runs, kMaxRuns);

  if (n == 0) {
    frameStats.skipped++;
    return;
  }
  lcd.teWait(100);  // TE 同步实验：等 V-blank 起点再开始写（超时 100ms 就照写，不卡死）
  frameStats.drawn++;
  if (n < 0) {  // 首帧 / 区间过多：整屏
    u8g2->sendBuffer();
    frameStats.full++;
    frameStats.bytes += static_cast<uint32_t>(size);
  } else {
    for (int i = 0; i < n; i++) u8g2->updateDisplayArea(runs[i].tx, runs[i].ty, runs[i].tw, 1);
    frameStats.partial++;
    frameStats.bytes += framediff::runBytes(runs, n);
  }
  memcpy(prevFrame, cur, size);
  hasPrevFrame = true;
}

void logFrameStats(uint32_t now) {
  if (now - lastFrameStatsMs < 60000) return;
  lastFrameStatsMs = now;
  Serial.printf("[ui] 近60秒：画了%lu帧（局部%lu/整屏%lu） 跳过%lu帧 发送%lu字节 TE等到%lu次 超时%lu次\n",
                static_cast<unsigned long>(frameStats.drawn), static_cast<unsigned long>(frameStats.partial),
                static_cast<unsigned long>(frameStats.full), static_cast<unsigned long>(frameStats.skipped),
                static_cast<unsigned long>(frameStats.bytes), static_cast<unsigned long>(lcd.teWaits),
                static_cast<unsigned long>(lcd.teTimeouts));
  lcd.teWaits = 0;
  lcd.teTimeouts = 0;
  frameStats.reset();
}

void paintTo() {
  u8g2->clearBuffer();
  if (g.page == PAGE_MONITOR_0) {
    paintPage0();
  } else if (g.page == PAGE_MONITOR_1) {
    paintPage1();
  } else if (g.page == PAGE_PET) {
    paintPetPage();
  } else {
    paintListPage();
  }
  presentFrame();
}

}  // namespace

DashState g;

void uiBegin() {
  pinMode(PIN_BTN_PAGE, INPUT_PULLUP);
  pinMode(PIN_BTN_POWER, INPUT_PULLUP);

  lcd.begin(0, U8G2_R1);  // 横屏 400x300
  u8g2 = lcd.getU8g2();
  lcd.teBegin(PIN_RLCD_TE);  // TE 同步实验（见 ST7305_U8g2.cpp）
  u8g2->setFont(kFontSmall);
  u8g2->setDrawColor(1);

  paintTo();
  lastPaintMs = millis();
}

void uiLoop() {
  readButton(btnPage, onPageShort, onPageLong);
  readButton(btnPower, onPowerShort, nullptr);

  if (!g.screenOn) return;

  const uint32_t now = millis();
  logFrameStats(now);
  const bool secondTick = now - lastPaintMs >= powerIv().redrawMs;  // 市电 1s，电池 5s；换页/链路变化仍立即重绘
  if (!secondTick && g.link == lastLink && g.page == lastPage) return;

  lastPaintMs = now;
  lastLink = g.link;
  lastPage = g.page;
  paintTo();
}
