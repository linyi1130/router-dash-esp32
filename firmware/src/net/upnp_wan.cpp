#include "upnp_wan.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <stdio.h>
#include <string.h>

#include "../state.h"
#include "power.h"
#include "upnp_wan_logic.h"

#if __has_include("config.h")
#include "config.h"
#else
#error "Copy firmware/include/config.example.h to firmware/include/config.h before building."
#endif

// ---- 可选配置（config.h 里没写也能编译，见 config.example.h）----
#ifndef UPNP_SAMPLE_MS
#define UPNP_SAMPLE_MS 2000  // WAN 流量采样周期（毫秒）
#endif
#ifndef UPNP_DESC_URL
#define UPNP_DESC_URL ""  // 设备描述地址，如 "http://192.168.50.1:32795/rootDesc.xml"；空 = 用 SSDP 自动发现
#endif

namespace {

constexpr uint16_t kConnectTimeoutMs = 600;
constexpr uint16_t kReadTimeoutMs = 800;
constexpr uint32_t kSsdpWaitMs = 3000;       // 发出 M-SEARCH 后等待应答的时间
constexpr uint16_t kSsdpLocalPort = 50501;   // 固定本地端口，接收单播应答
constexpr uint32_t kBackoffBaseMs = 5000;    // 发现失败退避 5s -> 10s -> ... -> 60s
constexpr uint32_t kBackoffMaxMs = 60000;
constexpr uint8_t kRediscoverAfter = 3;      // 连续读取失败这么多次就重新发现（端口可能变了）
constexpr uint32_t kPrintMs = 10000;         // 串口打印外网速率的间隔
constexpr size_t kMaxDescBytes = 8192;

enum class Phase : uint8_t { Idle, SsdpWait, Ready };

Phase phase = Phase::Idle;
WiFiUDP udp;
bool udpOpen = false;
uint32_t nextAttemptAt = 0;   // Idle：下一次发现；Ready：下一次采样
uint32_t ssdpDeadline = 0;
uint8_t discoverFailures = 0;
uint8_t pollFailures = 0;
uint32_t lastGoodMs = 0;

char ctlHost[48] = "";
uint16_t ctlPort = 0;
char ctlPath[128] = "";

upnp::RateCalc rate;

bool timeReached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

void closeUdp() {
  if (udpOpen) {
    udp.stop();
    udpOpen = false;
  }
}

void markInvalid() {
  g.wanDownSpeed = 0;
  g.wanUpSpeed = 0;
  g.wanValid = false;
}

// 读取设备描述 XML，找到 WANCommonInterfaceConfig 的控制地址。
bool describe(const char* url) {
  char host[48], path[96];
  uint16_t port = 0;
  if (!upnp::parseHttpUrl(url, host, sizeof(host), port, path, sizeof(path))) {
    Serial.println("[upnp] 描述地址无法解析");
    return false;
  }
  String body;
  {
    HTTPClient http;
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kReadTimeoutMs);
    http.setReuse(false);
    if (!http.begin(host, port, path)) return false;
    const int code = http.GET();
    const int size = http.getSize();
    if (code == 200 && (size < 0 || size <= static_cast<int>(kMaxDescBytes))) {
      body = http.getString();
    }
    http.end();
  }
  if (body.length() == 0 || body.length() > kMaxDescBytes) return false;

  char ctl[96];
  if (!upnp::findControlUrl(body.c_str(), upnp::kWanCommonService, ctl, sizeof(ctl))) {
    Serial.println("[upnp] 描述里没有 WANCommonInterfaceConfig");
    return false;
  }
  if (strncmp(ctl, "http://", 7) == 0) {
    if (!upnp::parseHttpUrl(ctl, ctlHost, sizeof(ctlHost), ctlPort, ctlPath, sizeof(ctlPath))) return false;
  } else {
    strncpy(ctlHost, host, sizeof(ctlHost) - 1);
    ctlHost[sizeof(ctlHost) - 1] = '\0';
    ctlPort = port;
    snprintf(ctlPath, sizeof(ctlPath), "%s%s", ctl[0] == '/' ? "" : "/", ctl);
  }
  Serial.printf("[upnp] 发现 IGD：%s:%u%s\n", ctlHost, static_cast<unsigned>(ctlPort), ctlPath);
  return true;
}

// 对 WANCommonInterfaceConfig 发一个无参 SOAP 动作，取回 uint32 字段。
bool soapGet(const char* action, const char* tag, uint32_t& val) {
  char body[384], soapAction[128];
  if (upnp::buildSoapBody(action, upnp::kWanCommonService, body, sizeof(body)) == 0) return false;
  if (!upnp::buildSoapAction(action, upnp::kWanCommonService, soapAction, sizeof(soapAction))) return false;
  String resp;
  bool ok = false;
  {
    HTTPClient http;
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kReadTimeoutMs);
    http.setReuse(false);
    if (!http.begin(ctlHost, ctlPort, ctlPath)) return false;
    http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
    http.addHeader("SOAPAction", soapAction);
    const int code = http.POST(reinterpret_cast<uint8_t*>(body), strlen(body));
    if (code == 200) {
      resp = http.getString();
      ok = true;
    }
    http.end();
  }
  return ok && upnp::extractUint32(resp.c_str(), tag, val);
}

void failDiscovery() {
  closeUdp();
  phase = Phase::Idle;
  if (discoverFailures < 250) discoverFailures++;
  uint32_t backoff = kBackoffBaseMs;
  for (uint8_t i = 1; i < discoverFailures && backoff < kBackoffMaxMs; i++) backoff *= 2;
  if (backoff > kBackoffMaxMs) backoff = kBackoffMaxMs;
  nextAttemptAt = millis() + backoff;
  Serial.printf("[upnp] 发现失败，%lus 后重试\n", static_cast<unsigned long>(backoff / 1000));
}

void discoveryDone() {
  closeUdp();
  phase = Phase::Ready;
  discoverFailures = 0;
  pollFailures = 0;
  rate.reset();
  nextAttemptAt = millis();
}

void sendSearch(const IPAddress& to, const char* st) {
  char msg[256];
  const int n = snprintf(msg, sizeof(msg),
                         "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                         "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n",
                         st);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(msg)) return;
  if (udp.beginPacket(to, 1900)) {
    udp.write(reinterpret_cast<const uint8_t*>(msg), static_cast<size_t>(n));
    udp.endPacket();
  }
}

void startSsdp() {
  if (!udp.begin(kSsdpLocalPort)) {
    failDiscovery();
    return;
  }
  udpOpen = true;
  const IPAddress mcast(239, 255, 255, 250);
  const IPAddress gw = WiFi.gatewayIP();
  // 组播 + 直接问网关（单播），两种 ST，任何一个有应答即可。
  sendSearch(mcast, upnp::kIgdDeviceType);
  sendSearch(mcast, upnp::kWanCommonService);
  sendSearch(gw, upnp::kIgdDeviceType);
  sendSearch(gw, upnp::kWanCommonService);
  ssdpDeadline = millis() + kSsdpWaitMs;
  phase = Phase::SsdpWait;
}

// 处理 SSDP 应答；只接受来自默认网关（路由器）的描述地址，避免局域网里别的设备冒充。
bool handleSsdpReplies() {
  const String gwStr = WiFi.gatewayIP().toString();
  int guard = 0;
  while (udp.parsePacket() > 0 && guard++ < 8) {
    char buf[512];
    const int n = udp.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
    if (n <= 0) continue;
    buf[n] = '\0';
    char loc[128];
    if (!upnp::parseSsdpLocation(buf, loc, sizeof(loc))) continue;
    char host[48], path[96];
    uint16_t port = 0;
    if (!upnp::parseHttpUrl(loc, host, sizeof(host), port, path, sizeof(path))) continue;
    if (gwStr != host) continue;
    if (describe(loc)) return true;
  }
  return false;
}

void poll(uint32_t now) {
  uint32_t rx = 0, tx = 0;
  const bool ok = soapGet("GetTotalBytesReceived", "NewTotalBytesReceived", rx) &&
                  soapGet("GetTotalBytesSent", "NewTotalBytesSent", tx);
  now = millis();
  if (!ok) {
    if (pollFailures < 250) pollFailures++;
    if (pollFailures >= kRediscoverAfter) {
      Serial.println("[upnp] 连续读取失败，重新发现");
      phase = Phase::Idle;
      nextAttemptAt = now;  // 立即重新发现；失败后按退避
      pollFailures = 0;
    }
    return;
  }
  pollFailures = 0;
  int64_t down = 0, up = 0;
  switch (rate.update(rx, tx, now, down, up)) {
    case upnp::RateCalc::Ok:
      g.wanDownSpeed = down;
      g.wanUpSpeed = up;
      g.wanValid = true;
      lastGoodMs = now;
      break;
    case upnp::RateCalc::Implausible:
      Serial.println("[upnp] 计数器跳变（路由器重启？），丢弃一个样本");
      break;
    case upnp::RateCalc::Baseline:
      break;
  }
}

}  // namespace

void upnpBegin() {
  closeUdp();
  phase = Phase::Idle;
  discoverFailures = 0;
  pollFailures = 0;
  rate.reset();
  nextAttemptAt = millis();
}

void upnpLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();

  switch (phase) {
    case Phase::Idle:
      if (!timeReached(now, nextAttemptAt)) break;
      if (UPNP_DESC_URL[0]) {
        if (describe(UPNP_DESC_URL)) {
          discoveryDone();
        } else {
          failDiscovery();
        }
      } else {
        startSsdp();
      }
      break;
    case Phase::SsdpWait:
      if (handleSsdpReplies()) {
        discoveryDone();
      } else if (timeReached(now, ssdpDeadline)) {
        failDiscovery();
      }
      break;
    case Phase::Ready:
      if (timeReached(now, nextAttemptAt)) {
        nextAttemptAt = now + powerIv().upnpSampleMs;
        poll(now);
      }
      break;
  }

  // 长时间没有有效样本：外网速度清零，避免一直显示旧值。
  // 用最新的 millis() 和有符号比较：poll() 里更新的 lastGoodMs 比函数开头的 now 晚。
  if (g.wanValid && upnp::isStale(millis(), lastGoodMs, powerIv().upnpStaleMs)) markInvalid();

  // 每 10 秒打印一次外网速率，方便用串口核对。
  static uint32_t lastPrint = 0;
  const uint32_t t = millis();
  if (t - lastPrint >= kPrintMs) {
    lastPrint = t;
    Serial.printf("[upnp] WAN 下行 %lld B/s 上行 %lld B/s 有效=%d 状态=%s rx=%lu tx=%lu\n",
                  static_cast<long long>(g.wanDownSpeed), static_cast<long long>(g.wanUpSpeed),
                  g.wanValid ? 1 : 0,
                  phase == Phase::Ready ? "就绪" : (phase == Phase::SsdpWait ? "发现中" : "等待发现"),
                  static_cast<unsigned long>(rate.rx), static_cast<unsigned long>(rate.tx));
  }
}

void upnpDisconnect() {
  closeUdp();
  phase = Phase::Idle;
  rate.reset();
  markInvalid();
  nextAttemptAt = millis();
}

void upnpKick() {
  if (phase == Phase::Ready) nextAttemptAt = millis();
}
