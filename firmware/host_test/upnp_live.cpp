// 主机端实机只读验证：用与固件相同的 upnp_wan_logic.h，对真实路由器做 SSDP 发现 + 取描述 + 读 WAN 计数器。
// 只做 M-SEARCH、GET 描述 XML、SOAP Get*（只读）。不登录、不改任何设置。
// g++ -std=c++17 -Wall -Wextra -I../src/net upnp_live.cpp -o /tmp/upnp_live && /tmp/upnp_live [路由器IP] [采样数]
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

#include "upnp_wan_logic.h"

static uint32_t nowMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

static std::string httpRequest(const char* host, uint16_t port, const std::string& req) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  timeval tv{0, 800000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, host, &a.sin_addr);
  std::string out;
  if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
    send(s, req.data(), req.size(), 0);
    char buf[2048];
    ssize_t n;
    while ((n = recv(s, buf, sizeof(buf), 0)) > 0) out.append(buf, static_cast<size_t>(n));
  }
  close(s);
  const size_t hdr = out.find("\r\n\r\n");
  if (out.compare(0, 12, "HTTP/1.1 200") != 0 && out.compare(0, 12, "HTTP/1.0 200") != 0) return "";
  return hdr == std::string::npos ? "" : out.substr(hdr + 4);
}

int main(int argc, char** argv) {
  const char* gw = argc > 1 ? argv[1] : "192.168.50.1";
  const int samples = argc > 2 ? atoi(argv[2]) : 6;

  // 1. SSDP（单播到网关 + 组播），与固件相同的两种 ST
  int u = socket(AF_INET, SOCK_DGRAM, 0);
  timeval tv{0, 300000};
  setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  const char* sts[] = {upnp::kIgdDeviceType, upnp::kWanCommonService};
  const char* targets[] = {gw, "239.255.255.250"};
  for (const char* t : targets)
    for (const char* st : sts) {
      char msg[256];
      snprintf(msg, sizeof(msg), "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n", st);
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(1900);
      inet_pton(AF_INET, t, &a.sin_addr);
      sendto(u, msg, strlen(msg), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }
  char desc[128] = "";
  const uint32_t t0 = nowMs();
  while (nowMs() - t0 < 3000 && !desc[0]) {
    char buf[1024];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    const ssize_t n = recvfrom(u, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n <= 0) continue;
    buf[n] = 0;
    char loc[128], host[48], path[96];
    uint16_t port;
    if (!upnp::parseSsdpLocation(buf, loc, sizeof(loc))) continue;
    if (!upnp::parseHttpUrl(loc, host, sizeof(host), port, path, sizeof(path))) continue;
    if (strcmp(host, gw) != 0) { printf("忽略非网关应答 %s\n", host); continue; }
    strcpy(desc, loc);
  }
  close(u);
  if (!desc[0]) { printf("FAIL: SSDP 未发现网关 IGD\n"); return 2; }
  printf("SSDP 发现耗时 %u ms，描述地址 %s\n", nowMs() - t0, desc);

  // 2. 取描述，找 controlURL
  char host[48], path[96], ctl[96], cpath[96];
  uint16_t port;
  upnp::parseHttpUrl(desc, host, sizeof(host), port, path, sizeof(path));
  const std::string xml = httpRequest(host, port, std::string("GET ") + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");
  if (!upnp::findControlUrl(xml.c_str(), upnp::kWanCommonService, ctl, sizeof(ctl))) { printf("FAIL: 描述里没有 WANCommonInterfaceConfig\n"); return 3; }
  snprintf(cpath, sizeof(cpath), "%s%s", ctl[0] == '/' ? "" : "/", ctl);
  printf("描述 %zu 字节，controlURL=%s\n", xml.size(), cpath);

  // 3. 采样
  auto soap = [&](const char* action, const char* tag, uint32_t& v) {
    char body[384], sa[128];
    upnp::buildSoapBody(action, upnp::kWanCommonService, body, sizeof(body));
    upnp::buildSoapAction(action, upnp::kWanCommonService, sa, sizeof(sa));
    std::string req = std::string("POST ") + cpath + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: " + sa +
                      "\r\nContent-Length: " + std::to_string(strlen(body)) + "\r\nConnection: close\r\n\r\n" + body;
    const uint32_t t = nowMs();
    const std::string r = httpRequest(host, port, req);
    printf("  %s %ums ", action, nowMs() - t);
    return upnp::extractUint32(r.c_str(), tag, v);
  };
  upnp::RateCalc rc;
  for (int i = 0; i < samples; i++) {
    uint32_t rx = 0, tx = 0;
    const bool ok = soap("GetTotalBytesReceived", "NewTotalBytesReceived", rx) && soap("GetTotalBytesSent", "NewTotalBytesSent", tx);
    if (!ok) { printf("\nFAIL: SOAP 读取失败\n"); return 4; }
    int64_t d = 0, up = 0;
    const auto r = rc.update(rx, tx, nowMs(), d, up);
    if (r == upnp::RateCalc::Ok) printf("-> 下行 %.0f kB/s  上行 %.0f kB/s\n", d / 1000.0, up / 1000.0);
    else printf("-> %s\n", r == upnp::RateCalc::Baseline ? "基线（首样本，不显示速率）" : "计数器跳变，丢弃");
    if (i + 1 < samples) std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  }
  printf("OK\n");
  return 0;
}
