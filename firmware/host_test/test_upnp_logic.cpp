// 主机端单元测试：g++ -std=c++17 -Wall -Wextra -I../src/net test_upnp_logic.cpp -o /tmp/test_upnp_logic
#include <assert.h>
#include <stdio.h>

#include <fstream>
#include <sstream>
#include <string>

#include "upnp_wan_logic.h"

static int checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static std::string readFile(const char* p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main() {
  using namespace upnp;

  // ---- 回绕差 ----
  CHECK(delta32(100, 40) == 60);
  CHECK(delta32(5, 0xFFFFFFFBu) == 10);            // 回绕：2^32-5 -> 5，增量 10
  CHECK(delta32(0, 0xFFFFFFFFu) == 1);
  CHECK(delta32(7, 7) == 0);

  // ---- 速率：正常 / 回绕 / 基线 ----
  {
    RateCalc rc; int64_t d = -1, u = -1;
    CHECK(rc.update(1000, 500, 10000, d, u) == RateCalc::Baseline);   // 第一次只记基线
    CHECK(rc.update(1000 + 2000000, 500 + 100000, 12000, d, u) == RateCalc::Ok);
    CHECK(d == 1000000 && u == 50000);                               // 2 MB/2s, 100 kB/2s
    // 回绕：下行从 2^32-1000000 走到 1000000（实际增量 2,000,000）
    rc.reset();
    CHECK(rc.update(0xFFFFFFFFu - 999999u, 0, 0, d, u) == RateCalc::Baseline);
    CHECK(rc.update(1000000u, 0, 2000, d, u) == RateCalc::Ok);
    CHECK(d == 1000000 && u == 0);
  }
  // ---- 计数器重置（路由器重启）：丢弃并重新基线，下一个样本恢复 ----
  {
    RateCalc rc; int64_t d = 0, u = 0;
    rc.update(1700000000u, 900000000u, 0, d, u);
    CHECK(rc.update(1700001000u, 900000100u, 2000, d, u) == RateCalc::Ok);
    CHECK(rc.update(1234u, 99u, 4000, d, u) == RateCalc::Implausible);  // 重启后计数从小值开始
    CHECK(rc.update(1234u + 4000u, 99u + 200u, 6000, d, u) == RateCalc::Ok);
    CHECK(d == 2000 && u == 100);
  }
  // ---- 间隔过长：只当新基线 ----
  {
    RateCalc rc; int64_t d = 0, u = 0;
    rc.update(10, 10, 0, d, u);
    CHECK(rc.update(20, 20, kMaxGapMs + 1, d, u) == RateCalc::Baseline);
    CHECK(rc.update(20, 20, kMaxGapMs + 1, d, u) == RateCalc::Baseline);  // dt == 0
  }
  // ---- millis() 回绕不影响 dt ----
  {
    RateCalc rc; int64_t d = 0, u = 0;
    rc.update(0, 0, 0xFFFFFFFFu - 999u, d, u);
    CHECK(rc.update(1000, 2000, 1000u, d, u) == RateCalc::Ok);          // dt = 2000ms
    CHECK(d == 500 && u == 1000);
  }
  // ---- 合理上限：刚好在上限内可接受 ----
  {
    RateCalc rc; int64_t d = 0, u = 0;
    rc.update(0, 0, 0, d, u);
    CHECK(rc.update(0x20000000u, 0, 2000, d, u) == RateCalc::Ok);       // 536 MB / 2s = 268 MB/s < 400 MB/s
    CHECK(rc.update(0x20000000u + 0x40000000u, 0, 4000, d, u) == RateCalc::Implausible);  // 1.07 GB/2s
  }

  // ---- 过期判断（回归：曾因无符号回绕导致外网速度刚算出就被清零）----
  {
    const uint32_t now = 100000;
    CHECK(!isStale(now, now + 8, 10000));            // lastGood 比 now 晚几毫秒：绝不能判为过期
    CHECK(!isStale(now, now, 10000));
    CHECK(!isStale(now, now - 10000, 10000));        // 刚好 10s 不算过期
    CHECK(isStale(now, now - 10001, 10000));
    CHECK(!isStale(5, 0xFFFFFFF0u, 10000));          // millis() 回绕后 21ms
    CHECK(isStale(20000, 0xFFFFFFF0u, 10000));       // 回绕后 20s
    // 对照：旧写法（无符号）在 lastGood 稍晚于 now 时会误判，这是 bug 的原样复现
    CHECK((now - (now + 8)) > 10000u);
  }

  // ---- SSDP LOCATION ----
  {
    char out[128];
    const char* r1 = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                     "USN: uuid:x::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nEXT:\r\nServer: AsusWRT/4.19.183 UPnP/1.1 MiniUPnPd/2.3.8\r\n"
                     "Location: http://192.168.50.1:32795/rootDesc.xml\r\nOPT: \"http://schemas.upnp.org/upnp/1/0/\"; ns=01\r\n\r\n";
    CHECK(parseSsdpLocation(r1, out, sizeof(out)) && std::string(out) == "http://192.168.50.1:32795/rootDesc.xml");
    CHECK(parseSsdpLocation("HTTP/1.1 200 OK\r\nlocation:   http://a/b  \r\n\r\n", out, sizeof(out)) && std::string(out) == "http://a/b");
    CHECK(!parseSsdpLocation("HTTP/1.1 200 OK\r\nX-Location: http://evil/\r\n\r\n", out, sizeof(out)));
    CHECK(!parseSsdpLocation("HTTP/1.1 200 OK\r\n\r\n", out, sizeof(out)));
    char tiny[8];
    CHECK(!parseSsdpLocation(r1, tiny, sizeof(tiny)));
  }

  // ---- URL ----
  {
    char host[48], path[96]; uint16_t port = 0;
    CHECK(parseHttpUrl("http://192.168.50.1:32795/rootDesc.xml", host, sizeof(host), port, path, sizeof(path)));
    CHECK(std::string(host) == "192.168.50.1" && port == 32795 && std::string(path) == "/rootDesc.xml");
    CHECK(parseHttpUrl("http://10.0.0.1/x?y=1", host, sizeof(host), port, path, sizeof(path)));
    CHECK(port == 80 && std::string(path) == "/x?y=1");
    CHECK(parseHttpUrl("http://10.0.0.1:5000", host, sizeof(host), port, path, sizeof(path)));
    CHECK(port == 5000 && std::string(path) == "/");
    CHECK(!parseHttpUrl("https://10.0.0.1/", host, sizeof(host), port, path, sizeof(path)));
    CHECK(!parseHttpUrl("http://10.0.0.1:99999/", host, sizeof(host), port, path, sizeof(path)));
    CHECK(!parseHttpUrl("http://10.0.0.1:/", host, sizeof(host), port, path, sizeof(path)));
    CHECK(!parseHttpUrl("http:///x", host, sizeof(host), port, path, sizeof(path)));
  }

  // ---- 真实设备描述（已脱敏）----
  {
    const std::string xml = readFile("rootDesc.sample.xml");
    CHECK(xml.size() > 1000);
    char ctl[96];
    CHECK(findControlUrl(xml.c_str(), kWanCommonService, ctl, sizeof(ctl)));
    CHECK(std::string(ctl) == "/ctl/CmnIfCfg");
    CHECK(findControlUrl(xml.c_str(), "urn:schemas-upnp-org:service:WANIPConnection:1", ctl, sizeof(ctl)));
    CHECK(std::string(ctl) == "/ctl/IPConn");
    CHECK(!findControlUrl(xml.c_str(), "urn:schemas-upnp-org:service:Nope:1", ctl, sizeof(ctl)));
    // 缺 controlURL 时不能串到下一个 service 的 controlURL
    const char* bad = "<service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType></service>"
                      "<service><serviceType>x</serviceType><controlURL>/other</controlURL></service>";
    CHECK(!findControlUrl(bad, kWanCommonService, ctl, sizeof(ctl)));
    // 绝对地址
    const char* abs = "<service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>"
                      "<controlURL>http://192.168.50.1:1234/c</controlURL></service>";
    CHECK(findControlUrl(abs, kWanCommonService, ctl, sizeof(ctl)) && std::string(ctl) == "http://192.168.50.1:1234/c");
  }

  // ---- SOAP 响应解析（真实响应格式）----
  {
    const char* resp = "<?xml version=\"1.0\"?>\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:GetTotalBytesReceivedResponse "
                       "xmlns:u=\"urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1\"><NewTotalBytesReceived>1759476135"
                       "</NewTotalBytesReceived></u:GetTotalBytesReceivedResponse></s:Body></s:Envelope>\r\n";
    uint32_t v = 0;
    CHECK(extractUint32(resp, "NewTotalBytesReceived", v) && v == 1759476135u);
    CHECK(!extractUint32(resp, "NewTotalBytesSent", v));
    CHECK(extractUint32("<NewTotalBytesSent>4294967295</NewTotalBytesSent>", "NewTotalBytesSent", v) && v == 4294967295u);
    CHECK(!extractUint32("<NewTotalBytesSent>4294967296</NewTotalBytesSent>", "NewTotalBytesSent", v));  // 超 32 位
    CHECK(!extractUint32("<NewTotalBytesSent>12ab</NewTotalBytesSent>", "NewTotalBytesSent", v));
    CHECK(!extractUint32("<NewTotalBytesSent>-5</NewTotalBytesSent>", "NewTotalBytesSent", v));
    CHECK(!extractUint32("<NewTotalBytesSent></NewTotalBytesSent>", "NewTotalBytesSent", v));
    char b[384], a[128];
    CHECK(buildSoapBody("GetTotalBytesSent", kWanCommonService, b, sizeof(b)) > 100);
    CHECK(std::string(b).find("<u:GetTotalBytesSent xmlns:u=\"urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1\"/>") != std::string::npos);
    CHECK(buildSoapAction("GetTotalBytesSent", kWanCommonService, a, sizeof(a)));
    CHECK(std::string(a) == "\"urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1#GetTotalBytesSent\"");
    char small[20];
    CHECK(buildSoapBody("GetTotalBytesSent", kWanCommonService, small, sizeof(small)) == 0);
  }

  printf("OK: %d checks passed\n", checks);
  return 0;
}
