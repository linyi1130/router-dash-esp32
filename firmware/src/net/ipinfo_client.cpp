#include "ipinfo_client.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include <ArduinoJson.h>
#include <string.h>

#include "../state.h"
#include "power.h"

namespace {


uint32_t lastRefresh = 0;

// myip.ipip.net 走 Clash DIRECT 分流，返回国内出口 IP 与中文归属地。
// 返回形如："当前 IP：203.0.113.10  来自于：中国 某省 某市  某运营商"（全角冒号分隔）。
bool fetchChina(String& ip, String& loc) {
  HTTPClient http;
  http.setTimeout(6000);
  http.setConnectTimeout(4000);
  http.setReuse(false);
  http.begin("http://myip.ipip.net");
  http.addHeader("User-Agent", "Mozilla/5.0");
  const int code = http.GET();
  bool ok = false;
  if (code == 200) {
    const String body = http.getString();
    const int c1 = body.indexOf("：");  // 第一个全角冒号（IP 之后）
    if (c1 >= 0) {
      int s = c1 + 3;  // 跳过全角冒号（3 字节）
      int e = s;
      while (e < static_cast<int>(body.length()) && body[e] != ' ' &&
             body[e] != '\r' && body[e] != '\n') {
        e++;
      }
      ip = body.substring(s, e);
    }
    const int c2 = body.indexOf("：", c1 + 1);  // 第二个全角冒号（来自于之后）
    if (c2 >= 0) {
      loc = body.substring(c2 + 3);
      loc.trim();
    }
    ok = ip.length() > 0;
  }
  http.end();
  return ok;
}

// ip-api.com 走 Clash PROXY 分流，返回全球节点出口 IP 与归属地。
bool fetchGlobal(String& ip, String& loc) {
  HTTPClient http;
  http.setTimeout(6000);
  http.setConnectTimeout(4000);
  http.setReuse(false);
  http.begin("http://ip-api.com/json?fields=status,country,city,query");
  http.addHeader("User-Agent", "curl/8.0.1");
  const int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      const String q = doc["query"] | "";
      if (q.length() > 0) {
        ip = q;
        const String country = doc["country"] | "";
        const String city = doc["city"] | "";
        loc = country + " " + city;  // 只留国家 + 城市，去掉 org（太长，放不下）
        loc.trim();
        ok = true;
      }
    }
  }
  http.end();
  return ok;
}

void store(char* dst, size_t cap, const String& src) {
  if (src.length() == 0) return;
  strncpy(dst, src.c_str(), cap - 1);
  dst[cap - 1] = '\0';
  normalizeGlyphs(dst);  // 压缩多余空格（ipip 返回的归属地里有双空格）
}

}  // namespace

void ipinfoBegin() {
  lastRefresh = millis() - powerIv().ipinfoRefreshMs;  // 立即触发首次查询
}

void ipinfoLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (now - lastRefresh < powerIv().ipinfoRefreshMs) return;
  lastRefresh = now;

  String ip, loc;
  if (fetchChina(ip, loc)) {
    store(g.chinaIp, sizeof(g.chinaIp), ip);
    store(g.chinaLoc, sizeof(g.chinaLoc), loc);
  }
  if (fetchGlobal(ip, loc)) {
    store(g.globalIp, sizeof(g.globalIp), ip);
    store(g.globalLoc, sizeof(g.globalLoc), loc);
  }
}

void ipinfoKick() { lastRefresh = millis() - powerIv().ipinfoRefreshMs; }
