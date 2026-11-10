#include "codex_client.h"

#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

#include "power.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy firmware/include/config.example.h to firmware/include/config.h before building."
#endif

// ---- 可选配置（config.h 里没写也能编译，见 config.example.h）----
#ifndef CODEX_ENABLED
#define CODEX_ENABLED 1
#endif
#ifndef CODEX_MDNS_HOST
#define CODEX_MDNS_HOST "codex-status"  // sidecar 用 dns-sd 广播的名字，不带 .local
#endif
#ifndef CODEX_SIDECAR_HOST
#define CODEX_SIDECAR_HOST ""  // mDNS 失败时用的固定地址（建议在路由器给 Mac 做 DHCP 保留）
#endif
#ifndef CODEX_SIDECAR_PORT
#define CODEX_SIDECAR_PORT 8787
#endif
#ifndef CODEX_POLL_MS
#define CODEX_POLL_MS 5000
#endif
#ifndef CODEX_STALE_MS
#define CODEX_STALE_MS 20000
#endif
#ifndef CODEX_TOKEN
#define CODEX_TOKEN ""  // 与 sidecar 的 CODEX_STATUS_TOKEN 一致；空 = 不带令牌头
#endif

namespace {

constexpr uint32_t kConnectTimeoutMs = 800;
constexpr uint32_t kReadTimeoutMs = 800;
constexpr uint32_t kRediscoverMs = 60000;   // 连续失败后重新解析 mDNS 的最短间隔
constexpr uint32_t kNoHostRetryMs = 30000;  // 一直没有地址时的重试间隔
constexpr uint32_t kMdnsQueryMs = 1000;
constexpr uint8_t kRediscoverAfter = 3;

char sidecarHost[64] = "";
uint16_t sidecarPort = CODEX_SIDECAR_PORT;
bool haveHost = false;
bool mdnsReady = false;
uint32_t lastDiscoverAt = 0;
uint32_t nextPollAt = 0;
uint8_t failures = 0;

bool timeReached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

// 先试 mDNS（codex-status.local），失败再用固定地址。
bool discover() {
  lastDiscoverAt = millis();
  if (!mdnsReady) mdnsReady = MDNS.begin("router-dash");
  if (mdnsReady && CODEX_MDNS_HOST[0]) {
    const IPAddress ip = MDNS.queryHost(CODEX_MDNS_HOST, kMdnsQueryMs);
    if (ip != IPAddress(static_cast<uint32_t>(0))) {
      strlcpy(sidecarHost, ip.toString().c_str(), sizeof(sidecarHost));
      sidecarPort = CODEX_SIDECAR_PORT;
      Serial.printf("[codex] mDNS %s -> %s\n", CODEX_MDNS_HOST, sidecarHost);
      return true;
    }
  }
  if (CODEX_SIDECAR_HOST[0]) {
    strlcpy(sidecarHost, CODEX_SIDECAR_HOST, sizeof(sidecarHost));
    sidecarPort = CODEX_SIDECAR_PORT;
    Serial.println("[codex] 使用固定地址");
    return true;
  }
  return false;
}

void copyClean(char* dst, size_t cap, const char* src) {
  char tmp[96];
  sanitizeUtf8(tmp, sizeof(tmp), src);
  normalizeGlyphs(tmp);
  strlcpy(dst, tmp, cap);
}

// 解析 /status。只取需要的字段（ArduinoJson filter），不取 summary。
bool applyJson(const String& body) {
  JsonDocument filter;
  filter["label"] = true;
  filter["status"] = true;
  filter["project_name"] = true;
  filter["window_name"] = true;
  filter["weekly_used_percent"] = true;
  filter["weekly_reset_text"] = true;
  filter["plan_name"] = true;
  filter["context_used_tokens"] = true;
  filter["context_limit_tokens"] = true;
  filter["context_used_percent"] = true;
  filter["context_text"] = true;
  filter["turn_elapsed_seconds"] = true;
  filter["turn_started_at"] = true;
  filter["unix"] = true;
  filter["tz_offset_seconds"] = true;
  filter["has_session"] = true;
  filter["grok"]["state"] = true;
  filter["grok"]["weekly_percent"] = true;
  filter["grok"]["next_reset_unix"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    Serial.println("[codex] JSON 解析失败");
    return false;
  }
  const int64_t unixNow = doc["unix"] | static_cast<int64_t>(0);
  if (unixNow <= 0) return false;  // 不是 sidecar 的正常响应

  CodexState& c = g.codex;
  copyClean(c.status, sizeof(c.status), doc["status"] | "idle");
  copyClean(c.label, sizeof(c.label), doc["label"] | "");
  const char* windowName = doc["window_name"] | "";
  const char* project = doc["project_name"] | "";
  copyClean(c.window, sizeof(c.window), windowName[0] ? windowName : project);

  char reset[40];
  char plan[16];
  copyClean(reset, sizeof(reset), doc["weekly_reset_text"] | "");
  copyClean(plan, sizeof(plan), doc["plan_name"] | "");
  if (reset[0] && plan[0]) snprintf(c.footer, sizeof(c.footer), "%s %s", reset, plan);
  else if (reset[0]) strlcpy(c.footer, reset, sizeof(c.footer));
  else if (plan[0]) strlcpy(c.footer, plan, sizeof(c.footer));
  else strlcpy(c.footer, "暂无数据", sizeof(c.footer));

  // 「349k / 475k tokens」去掉结尾 tokens，面板里放得下。
  char ctx[28];
  copyClean(ctx, sizeof(ctx), doc["context_text"] | "");
  const size_t n = strlen(ctx);
  if (n > 7 && strcmp(ctx + n - 7, " tokens") == 0) ctx[n - 7] = '\0';
  strlcpy(c.ctxText, ctx[0] ? ctx : "--", sizeof(c.ctxText));

  c.weekly = doc["weekly_used_percent"].isNull() ? -1.0f : doc["weekly_used_percent"].as<float>();
  const long used = doc["context_used_tokens"].isNull() ? -1 : doc["context_used_tokens"].as<long>();
  const long limit = doc["context_limit_tokens"].isNull() ? -1 : doc["context_limit_tokens"].as<long>();
  if (used >= 0 && limit > 0) c.context = static_cast<float>(used) * 100.0f / static_cast<float>(limit);
  else if (!doc["context_used_percent"].isNull()) c.context = doc["context_used_percent"].as<float>();
  else c.context = -1.0f;

  c.hasSession = doc["has_session"] | false;
  // 真实数据里额度高时 status 仍是 complete/running，只有 label 写「额度将尽」（可能带后缀）。
  c.quota = strcmp(c.status, "quota") == 0 || strncmp(c.label, "额度将尽", 12) == 0;
  c.elapsedSeconds = doc["turn_elapsed_seconds"] | 0;
  c.turnStartedAt = doc["turn_started_at"].isNull() ? 0 : doc["turn_started_at"].as<int64_t>();
  c.unixBase = unixNow;
  c.tzOffset = doc["tz_offset_seconds"] | 0;
  c.syncMs = millis();
  c.everSynced = true;

  // Grok Bot 用量。percent 为 null（读不到）时保留上次的值，应用关闭时仍显示缓存用量。
  const char* gs = doc["grok"]["state"] | "";
  if (strcmp(gs, "ok") == 0) c.grokState = GROK_OK;
  else if (strcmp(gs, "closed") == 0) c.grokState = GROK_CLOSED;
  else if (gs[0]) c.grokState = GROK_UNAVAILABLE;
  else c.grokState = GROK_UNKNOWN;
  if (!doc["grok"]["weekly_percent"].isNull()) c.grokPercent = doc["grok"]["weekly_percent"].as<float>();
  if (!doc["grok"]["next_reset_unix"].isNull()) c.grokResetUnix = doc["grok"]["next_reset_unix"].as<int64_t>();
  return true;
}

// 返回 HTTP 状态码；200 且解析成功返回 200，其他返回错误码/负数。
int fetchStatus() {
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kReadTimeoutMs);
  char url[96];
  snprintf(url, sizeof(url), "http://%s:%u/status", sidecarHost, static_cast<unsigned>(sidecarPort));
  if (!http.begin(url)) return -1;
  if (CODEX_TOKEN[0]) http.addHeader("X-Status-Token", CODEX_TOKEN);
  const int code = http.GET();
  int result = code;
  if (code == 200) {
    const String body = http.getString();
    result = applyJson(body) ? 200 : -2;
  }
  http.end();
  return result;
}

void onSuccess(uint32_t now) {
  failures = 0;
  g.codex.authFailed = false;
  g.codex.lastOkMs = now;
  nextPollAt = now + powerIv().codexPollMs;
}

void onFailure(uint32_t now, int code) {
  if (failures < 250) failures++;
  g.codex.authFailed = (code == 401);
  // 市电档 5s -> 10s -> 20s -> 30s；电池档 30s -> 60s -> 120s（上限随档位变）。
  const uint32_t backoffMax = powerIv().codexBackoffMaxMs;
  uint32_t backoff = powerIv().codexPollMs;
  for (uint8_t i = 1; i < failures && backoff < backoffMax; i++) backoff *= 2;
  if (backoff > backoffMax) backoff = backoffMax;
  nextPollAt = now + backoff;
  Serial.printf("[codex] 拉取失败 code=%d，%lus 后重试\n", code, static_cast<unsigned long>(backoff / 1000));
  if (failures >= kRediscoverAfter && timeReached(now, lastDiscoverAt + kRediscoverMs)) {
    haveHost = discover() || haveHost;  // 重新解析失败时继续用旧地址
  }
}

}  // namespace

void codexBegin() {
#if CODEX_ENABLED
  failures = 0;
  haveHost = discover();
  nextPollAt = millis();
#endif
}

void codexDisconnect() {
  // 数据保留（面板显示「连接中断」+ 上次数据）；WiFi 回来后 codexBegin() 重新解析地址。
  haveHost = false;
}

void codexLoop() {
#if CODEX_ENABLED
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (!haveHost) {
    if (!timeReached(now, lastDiscoverAt + kNoHostRetryMs)) return;
    haveHost = discover();
    if (!haveHost) return;
    nextPollAt = now;
  }
  if (!timeReached(now, nextPollAt)) return;
  const int code = fetchStatus();
  if (code == 200) onSuccess(millis());
  else onFailure(millis(), code);
#endif
}

CodexLink codexLinkNow() {
  const CodexState& c = g.codex;
  if (!c.everSynced) return CODEX_NEVER;
  // 电池档拉取间隔变长，「连接中断」阈值按倍数放宽（6 倍 = 与 5s→30s 成比例）。
  if (millis() - c.lastOkMs > static_cast<uint32_t>(CODEX_STALE_MS) * powerIv().codexStaleScale) return CODEX_STALE;
  return CODEX_OK;
}

int64_t codexUnixNow() {
  const CodexState& c = g.codex;
  if (!c.everSynced) return 0;
  return c.unixBase + static_cast<int64_t>((millis() - c.syncMs) / 1000UL);
}

void codexKick() {
#if CODEX_ENABLED
  if (haveHost) nextPollAt = millis();
#endif
}
