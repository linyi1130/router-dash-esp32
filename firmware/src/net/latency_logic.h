#pragma once
// 经 Clash 延迟接口测速的纯逻辑（不依赖 Arduino），固件和主机端测试共用。
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace latency {

// 百分号编码（RFC 3986 unreserved 之外全部编码），返回写入长度，放不下返回 0。
inline size_t urlEncode(const char* s, char* out, size_t cap) {
  size_t o = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); p && *p; p++) {
    const unsigned char c = *p;
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '-' || c == '_' || c == '.' || c == '~';
    if (plain) {
      if (o + 2 > cap) return 0;
      out[o++] = static_cast<char>(c);
    } else {
      if (o + 4 > cap) return 0;
      snprintf(out + o, 4, "%%%02X", c);
      o += 3;
    }
  }
  if (o + 1 > cap) return 0;
  out[o] = '\0';
  return o;
}

// /proxies/<代理名>/delay?url=<站点>&timeout=<毫秒>
inline bool buildDelayPath(const char* proxy, const char* url, uint32_t timeoutMs, char* out, size_t cap) {
  char p[160], u[160];
  if (urlEncode(proxy, p, sizeof(p)) == 0 || urlEncode(url, u, sizeof(u)) == 0) return false;
  const int n = snprintf(out, cap, "/proxies/%s/delay?url=%s&timeout=%lu", p, u,
                         static_cast<unsigned long>(timeoutMs));
  return n > 0 && static_cast<size_t>(n) < cap;
}

// 解析 Clash 延迟接口响应。成功 {"delay":123} 返回 123；
// {"message":"Timeout"} / 其它错误 / 解析失败 / 0 返回 -1（与屏幕「超时」语义一致）。
inline int parseDelayMs(int httpCode, const char* body) {
  if (httpCode != 200 || !body) return -1;
  const char* p = strstr(body, "\"delay\"");
  if (!p) return -1;
  p += 7;
  while (*p == ' ' || *p == ':' || *p == '\t') p++;
  if (*p < '0' || *p > '9') return -1;
  long v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > 60000) return -1;
    p++;
  }
  return v > 0 ? static_cast<int>(v) : -1;
}

}  // namespace latency
