#include "clash_client.h"

#include <HTTPClient.h>
#include <StreamString.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../state.h"
#include "power.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy firmware/include/config.example.h to firmware/include/config.h before building."
#endif

namespace {

constexpr uint32_t kBackoffMin = 1000;
constexpr uint32_t kBackoffMax = 30000;
constexpr uint32_t kMetaMs = 300000;  // /version + /proxies 每 5 分钟

// /traffic 与 /memory 长连接状态。
WiFiClient trafficClient;
WiFiClient memoryClient;
bool trafficLive = false;
bool memoryLive = false;
uint32_t trafficBackoff = kBackoffMin;
uint32_t memoryBackoff = kBackoffMin;
uint32_t lastTrafficAttempt = 0;
uint32_t lastMemoryAttempt = 0;
uint32_t lastConnections = 0;
uint32_t lastMeta = 0;

// 从流里按花括号配对提取完整 JSON 对象，兼容 chunked 编码的混入字节。
struct StreamParser {
  char buf[640];
  size_t len = 0;
  int depth = 0;

  void reset() {
    len = 0;
    depth = 0;
  }
};

StreamParser trafficParser;
StreamParser memoryParser;

String authHeader() {
  return String("Bearer ") + CLASH_SECRET;
}

String baseUrl() {
  return String("http://") + CLASH_HOST + ":" + CLASH_PORT;
}

// 发送 GET 并读到响应头结束，返回 HTTP 状态码；连接失败返回 0。
int openStream(WiFiClient& c, const char* path) {
  c.stop();
  if (!c.connect(CLASH_HOST, CLASH_PORT)) return 0;
  c.setNoDelay(true);
  c.setTimeout(4000);
  c.printf("GET %s HTTP/1.1\r\nHost: %s:%d\r\nAuthorization: %s\r\nConnection: keep-alive\r\n\r\n",
           path, CLASH_HOST, CLASH_PORT, authHeader().c_str());
  // 读状态行。
  String line = c.readStringUntil('\n');
  line.trim();
  int code = 0;
  if (line.startsWith("HTTP/")) {
    int sp = line.indexOf(' ');
    if (sp >= 0) code = atoi(line.c_str() + sp + 1);
  }
  // 跳过响应头直到空行。
  while (c.connected()) {
    String h = c.readStringUntil('\n');
    if (h == "\r" || h == "" || h == "\r\n") break;
  }
  return code;
}

template <typename Fn>
bool pumpObjects(WiFiClient& c, StreamParser& p, Fn&& onObject) {
  bool any = false;
  while (c.available() > 0) {
    char ch = static_cast<char>(c.read());
    if (p.depth == 0) {
      if (ch == '{') {
        p.len = 0;
        p.buf[p.len++] = ch;
        p.depth = 1;
      }
      continue;
    }
    if (p.len + 1 < sizeof(p.buf)) {
      p.buf[p.len++] = ch;
    } else {
      p.reset();  // 对象过大，放弃。
      continue;
    }
    if (ch == '{') {
      p.depth++;
    } else if (ch == '}') {
      p.depth--;
      if (p.depth == 0) {
        p.buf[p.len] = '\0';
        onObject(p.buf, p.len);
        any = true;
      }
    }
  }
  return any;
}

void onTraffic(const char* json, size_t len) {
  static JsonDocument doc;  // 复用内存池，避免每秒分配/释放。
  if (deserializeJson(doc, json, len)) return;
  g.upSpeed = doc["up"].as<int64_t>();
  g.downSpeed = doc["down"].as<int64_t>();
  g.link = LINK_OK;
}

void onMemory(const char* json, size_t len) {
  static JsonDocument doc;
  if (deserializeJson(doc, json, len)) return;
  int64_t inuse = doc["inuse"].as<int64_t>();
  if (inuse > 0) g.memInuse = inuse;  // 忽略首帧 0。
  g.link = LINK_OK;
}

void backoff(uint32_t& value) {
  value *= 2;
  if (value > kBackoffMax) value = kBackoffMax;
}

void ensureTraffic() {
  const uint32_t now = millis();
  if (trafficLive) {
    if (!trafficClient.connected() && trafficClient.available() == 0) {
      trafficLive = false;
      trafficClient.stop();
    } else {
      pumpObjects(trafficClient, trafficParser, onTraffic);
      return;
    }
  }
  if (now - lastTrafficAttempt < trafficBackoff) return;
  lastTrafficAttempt = now;
  const int code = openStream(trafficClient, "/traffic");
  if (code == 200) {
    trafficLive = true;
    trafficParser.reset();
    trafficBackoff = kBackoffMin;
    g.link = LINK_OK;
  } else if (code == 401) {
    g.link = LINK_401;
    trafficClient.stop();
    trafficBackoff = kBackoffMax;
  } else {
    g.link = LINK_DOWN;
    trafficClient.stop();
    backoff(trafficBackoff);
  }
}

void ensureMemory() {
  const uint32_t now = millis();
  if (memoryLive) {
    if (!memoryClient.connected() && memoryClient.available() == 0) {
      memoryLive = false;
      memoryClient.stop();
    } else {
      pumpObjects(memoryClient, memoryParser, onMemory);
      return;
    }
  }
  if (now - lastMemoryAttempt < memoryBackoff) return;
  lastMemoryAttempt = now;
  const int code = openStream(memoryClient, "/memory");
  if (code == 200) {
    memoryLive = true;
    memoryParser.reset();
    memoryBackoff = kBackoffMin;
    g.link = LINK_OK;
  } else if (code == 401) {
    g.link = LINK_401;
    memoryClient.stop();
    memoryBackoff = kBackoffMax;
  } else {
    g.link = LINK_DOWN;
    memoryClient.stop();
    backoff(memoryBackoff);
  }
}

void resetTop(TopHost& t) {
  t.host[0] = '\0';
  t.srcIP[0] = '\0';
  t.node[0] = '\0';
  t.bytes = 0;
}

void fillTop(TopHost& t, const char* host, const char* srcIP, const char* node, int64_t bytes) {
  sanitizeUtf8(t.host, sizeof(t.host), host);
  sanitizeUtf8(t.srcIP, sizeof(t.srcIP), srcIP);
  sanitizeUtf8(t.node, sizeof(t.node), node);
  t.bytes = bytes;
}

// GET 并返回响应体（已解 chunked）；非 200 时 body 为空。
String httpGetBody(const char* path, int& code) {
  HTTPClient http;
  http.setTimeout(6000);
  http.setConnectTimeout(3000);
  http.begin(baseUrl() + path);
  http.addHeader("Authorization", authHeader());
  code = http.GET();
  String body;
  if (code == 200) {
    StreamString s;
    http.writeToStream(&s);
    body = s;
  }
  http.end();
  return body;
}

void setLinkFromCode(int code) {
  if (code == 401) g.link = LINK_401;
  else if (code > 0) g.link = LINK_DOWN;
}

void fetchConnections() {
  int code = 0;
  const String body = httpGetBody("/connections", code);
  if (code != 200 || body.length() == 0) {
    setLinkFromCode(code);
    return;
  }

  JsonDocument filter;
  filter["downloadTotal"] = true;
  filter["uploadTotal"] = true;
  filter["memory"] = true;
  filter["connections"][0]["metadata"]["host"] = true;
  filter["connections"][0]["metadata"]["sniffHost"] = true;
  filter["connections"][0]["metadata"]["destinationIP"] = true;
  filter["connections"][0]["metadata"]["sourceIP"] = true;
  filter["connections"][0]["chains"][0] = true;
  filter["connections"][0]["download"] = true;
  filter["connections"][0]["upload"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    g.link = LINK_DOWN;
    return;
  }

  g.downloadTotal = doc["downloadTotal"].as<int64_t>();
  g.uploadTotal = doc["uploadTotal"].as<int64_t>();
  const int64_t mem = doc["memory"].as<int64_t>();
  if (mem > 0) g.memInuse = mem;

  JsonArray conns = doc["connections"].as<JsonArray>();
  g.connCount = conns.size();

  resetTop(g.topDown);
  resetTop(g.topUp);
  for (JsonObject c : conns) {
    const int64_t dl = c["download"].as<int64_t>();
    const int64_t ul = c["upload"].as<int64_t>();
    const char* host = c["metadata"]["host"] | "";
    if (!host[0]) host = c["metadata"]["sniffHost"] | "";
    if (!host[0]) host = c["metadata"]["destinationIP"] | "";
    const char* srcIP = c["metadata"]["sourceIP"] | "";
    const char* node = c["chains"][0] | "";
    if (dl > g.topDown.bytes) fillTop(g.topDown, host, srcIP, node, dl);
    if (ul > g.topUp.bytes) fillTop(g.topUp, host, srcIP, node, ul);
  }
  g.link = LINK_OK;
}

String urlEncode(const char* s) {
  String out;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; p++) {
    const unsigned char c = *p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

void fetchMeta() {
  // /version
  {
    int code = 0;
    const String body = httpGetBody("/version", code);
    if (code == 200 && body.length() > 0) {
      JsonDocument doc;
      if (!deserializeJson(doc, body)) {
        strlcpy(g.version, doc["version"] | "", sizeof(g.version));
      }
    } else if (code == 401) {
      g.link = LINK_401;
    }
  }

  // 当前节点：先 GET /group 找 Selector 组，再 GET /proxies/<组名>
  {
    int code = 0;
    String groupName;
    const String groupBody = httpGetBody("/group", code);
    if (code == 200 && groupBody.length() > 0) {
      JsonDocument doc;
      if (!deserializeJson(doc, groupBody)) {
        for (JsonObject p : doc["proxies"].as<JsonArray>()) {
          const char* t = p["type"] | "";
          if (strcmp(t, "Selector") != 0) continue;
          const char* nm = p["name"] | "";
          if (groupName.length() == 0) groupName = nm;              // 兜底：第一个 Selector
          if (strcmp(nm, PROXY_GROUP) == 0) { groupName = nm; break; }  // 优先配置的节点选择组
        }
      }
    } else if (code == 401) {
      g.link = LINK_401;
    }
    if (groupName.length() == 0) groupName = PROXY_GROUP;

    const String proxyPath = String("/proxies/") + urlEncode(groupName.c_str());
    int c2 = 0;
    const String proxyBody = httpGetBody(proxyPath.c_str(), c2);
    if (c2 == 200 && proxyBody.length() > 0) {
      JsonDocument doc;
      if (!deserializeJson(doc, proxyBody)) {
        sanitizeUtf8(g.currentNode, sizeof(g.currentNode), doc["now"] | "");
      }
    } else if (c2 == 401) {
      g.link = LINK_401;
    }
  }
}

}  // namespace

void clashBegin() {
  // 用「当前时间 - 间隔」让下一次 clashLoop 立即触发首次拉取，
  // 否则 lastMeta=0 会延迟到开机 5 分钟后才取节点/版本。
  lastConnections = millis() - powerIv().clashConnectionsMs;
  lastMeta = millis() - kMetaMs;
}

void clashLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  ensureTraffic();
  ensureMemory();

  const uint32_t now = millis();
  if (now - lastConnections >= powerIv().clashConnectionsMs) {
    lastConnections = now;
    fetchConnections();
  }
  if (now - lastMeta >= kMetaMs) {
    lastMeta = now;
    fetchMeta();
  }
}

void clashDisconnect() {
  trafficLive = false;
  memoryLive = false;
  trafficClient.stop();
  memoryClient.stop();
  trafficBackoff = kBackoffMin;
  memoryBackoff = kBackoffMin;
}

void clashKick() { lastConnections = millis() - powerIv().clashConnectionsMs; }
