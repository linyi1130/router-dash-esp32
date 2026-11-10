#pragma once
// UPnP IGD 读取 WAN 流量的纯逻辑（不依赖 Arduino / 网络），固件和主机端测试共用。
// 只含：SSDP/XML/URL 解析、SOAP 请求体生成、32 位计数器回绕与速率计算。
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace upnp {

constexpr const char* kIgdDeviceType = "urn:schemas-upnp-org:device:InternetGatewayDevice:1";
constexpr const char* kWanCommonService = "urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1";

// 单方向最大合理速率 400 MB/s（≈3.2 Gbit/s，高于 2.5G 口）。超过视为计数器被重置，丢弃该样本。
constexpr uint64_t kMaxBytesPerSec = 400ULL * 1000 * 1000;
// 两次样本间隔超过这个值（卡住/长时间失败）就只当新基线，因为 32 位计数器可能已回绕多次。
constexpr uint32_t kMaxGapMs = 120000;

// 32 位无符号回绕差：UPnP 的 TotalBytes 到 2^32-1 后回到 0。
inline uint32_t delta32(uint32_t cur, uint32_t prev) { return cur - prev; }

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// 不区分大小写的 strstr。
inline const char* ciFind(const char* hay, const char* needle) {
  if (!hay || !needle || !*needle) return nullptr;
  const size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && lowerAscii(hay[i]) == lowerAscii(needle[i])) i++;
    if (i == n) return hay;
  }
  return nullptr;
}

// 从 SSDP 响应里取 LOCATION 头的值。
inline bool parseSsdpLocation(const char* resp, char* out, size_t cap) {
  if (!resp || cap < 8) return false;
  const char* p = resp;
  while ((p = ciFind(p, "location:")) != nullptr) {
    if (p == resp || p[-1] == '\n') {  // 必须在行首
      p += 9;
      while (*p == ' ' || *p == '\t') p++;
      size_t n = 0;
      while (p[n] && p[n] != '\r' && p[n] != '\n') n++;
      while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t')) n--;
      if (n == 0 || n + 1 > cap) return false;
      memcpy(out, p, n);
      out[n] = '\0';
      return true;
    }
    p += 9;
  }
  return false;
}

// 解析 http://host[:port]/path；只支持 http。
inline bool parseHttpUrl(const char* url, char* host, size_t hcap, uint16_t& port, char* path,
                         size_t pcap) {
  if (!url || strncmp(url, "http://", 7) != 0) return false;
  const char* h = url + 7;
  size_t hn = 0;
  while (h[hn] && h[hn] != ':' && h[hn] != '/') hn++;
  if (hn == 0 || hn + 1 > hcap) return false;
  memcpy(host, h, hn);
  host[hn] = '\0';
  const char* q = h + hn;
  uint32_t prt = 80;
  if (*q == ':') {
    q++;
    prt = 0;
    int digits = 0;
    while (*q >= '0' && *q <= '9') {
      prt = prt * 10 + static_cast<uint32_t>(*q - '0');
      if (prt > 65535) return false;
      q++;
      digits++;
    }
    if (digits == 0 || prt == 0) return false;
  }
  port = static_cast<uint16_t>(prt);
  const char* pp = (*q == '/') ? q : "/";
  const size_t pn = strlen(pp);
  if (pn + 1 > pcap) return false;
  memcpy(path, pp, pn + 1);
  return true;
}

// 在设备描述 XML 里找指定 serviceType 的 controlURL（原样返回，可能是相对路径或完整 URL）。
inline bool findControlUrl(const char* xml, const char* serviceType, char* out, size_t cap) {
  if (!xml || !serviceType) return false;
  const size_t stLen = strlen(serviceType);
  const char* p = xml;
  while ((p = strstr(p, "<serviceType>")) != nullptr) {
    p += 13;
    const bool match = strncmp(p, serviceType, stLen) == 0 && strncmp(p + stLen, "</serviceType>", 14) == 0;
    const char* svcEnd = strstr(p, "</service>");
    if (!match) continue;
    const char* c = strstr(p, "<controlURL>");
    if (!c || (svcEnd && c > svcEnd)) return false;
    c += 12;
    const char* e = strstr(c, "</controlURL>");
    if (!e) return false;
    const size_t n = static_cast<size_t>(e - c);
    if (n == 0 || n + 1 > cap) return false;
    memcpy(out, c, n);
    out[n] = '\0';
    return true;
  }
  return false;
}

// 取 <tag>12345</tag> 里的无符号整数，必须是纯数字且 ≤ 2^32-1。
inline bool extractUint32(const char* xml, const char* tag, uint32_t& val) {
  if (!xml || !tag) return false;
  char open[48];
  const int on = snprintf(open, sizeof(open), "<%s>", tag);
  if (on <= 0 || static_cast<size_t>(on) >= sizeof(open)) return false;
  const char* p = strstr(xml, open);
  if (!p) return false;
  p += on;
  if (*p < '0' || *p > '9') return false;
  uint64_t v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + static_cast<uint64_t>(*p - '0');
    if (v > 0xFFFFFFFFULL) return false;
    p++;
  }
  if (*p != '<') return false;
  val = static_cast<uint32_t>(v);
  return true;
}

// 生成无参数 SOAP 请求体，返回长度（失败 0）。
inline size_t buildSoapBody(const char* action, const char* serviceType, char* buf, size_t cap) {
  const int n = snprintf(
      buf, cap,
      "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
      "<u:%s xmlns:u=\"%s\"/></s:Body></s:Envelope>",
      action, serviceType);
  return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

// SOAPAction 头的值（带引号）。
inline bool buildSoapAction(const char* action, const char* serviceType, char* buf, size_t cap) {
  const int n = snprintf(buf, cap, "\"%s#%s\"", serviceType, action);
  return n > 0 && static_cast<size_t>(n) < cap;
}

// 距离上一次有效样本是否已经过期。用有符号差：lastGood 可能比 now 稍晚（now 是更早取的时间），
// 无符号相减会回绕成巨大正数而误判过期（曾导致外网速度刚算出来就被清零）。
inline bool isStale(uint32_t now, uint32_t lastGoodMs, uint32_t staleMs) {
  return static_cast<int32_t>(now - lastGoodMs) > static_cast<int32_t>(staleMs);
}

// 两次累计值 -> 速率。
struct RateCalc {
  enum Result : uint8_t {
    Baseline,     // 第一次/间隔过长：只记录基线，没有速率
    Ok,           // 速率有效
    Implausible,  // 增量不合理（路由器重启/计数器被清零）：丢弃并重新记录基线
  };

  bool has = false;
  uint32_t rx = 0, tx = 0, atMs = 0;

  void reset() { has = false; }

  Result update(uint32_t curRx, uint32_t curTx, uint32_t nowMs, int64_t& downBps, int64_t& upBps) {
    const uint32_t dt = nowMs - atMs;
    const bool usable = has && dt > 0 && dt <= kMaxGapMs;
    const uint32_t dRx = delta32(curRx, rx);
    const uint32_t dTx = delta32(curTx, tx);
    rx = curRx;
    tx = curTx;
    atMs = nowMs;
    has = true;
    if (!usable) return Baseline;
    const uint64_t limit = kMaxBytesPerSec * dt / 1000;
    if (dRx > limit || dTx > limit) return Implausible;
    downBps = static_cast<int64_t>(static_cast<uint64_t>(dRx) * 1000 / dt);
    upBps = static_cast<int64_t>(static_cast<uint64_t>(dTx) * 1000 / dt);
    return Ok;
  }
};

}  // namespace upnp
