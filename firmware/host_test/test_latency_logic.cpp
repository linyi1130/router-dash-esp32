// g++ -std=c++17 -Wall -Wextra -I../src/net test_latency_logic.cpp -o /tmp/test_latency_logic
#include <stdio.h>

#include <string>

#include "latency_logic.h"

static int checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
  using namespace latency;

  // 真实 Clash（mihomo v1.19.3）响应
  CHECK(parseDelayMs(200, "{\"delay\":1001}\n") == 1001);
  CHECK(parseDelayMs(200, "{\"delay\":22}") == 22);
  CHECK(parseDelayMs(200, "{\"delay\": 366}") == 366);
  CHECK(parseDelayMs(504, "{\"message\":\"Timeout\"}\n") == -1);
  CHECK(parseDelayMs(503, "{\"message\":\"An error occurred in the delay test\"}\n") == -1);
  CHECK(parseDelayMs(401, "{\"message\":\"Unauthorized\"}") == -1);
  CHECK(parseDelayMs(404, "{\"message\":\"resource not found\"}") == -1);
  CHECK(parseDelayMs(200, "{\"delay\":0}") == -1);
  CHECK(parseDelayMs(200, "{\"delay\":-5}") == -1);
  CHECK(parseDelayMs(200, "{\"delay\":\"abc\"}") == -1);
  CHECK(parseDelayMs(200, "{\"delay\":999999}") == -1);
  CHECK(parseDelayMs(200, "") == -1);
  CHECK(parseDelayMs(200, nullptr) == -1);
  CHECK(parseDelayMs(-1, "{\"delay\":5}") == -1);       // HTTPClient 的负数错误码

  // 编码：与 Python urllib.parse.quote(safe="") 的结果一致
  char out[160];
  CHECK(urlEncode("\xF0\x9F\x9A\x80 \xE8\x8A\x82\xE7\x82\xB9\xE9\x80\x89\xE6\x8B\xA9", out, sizeof(out)) > 0);
  CHECK(std::string(out) == "%F0%9F%9A%80%20%E8%8A%82%E7%82%B9%E9%80%89%E6%8B%A9");
  CHECK(urlEncode("https://cp.cloudflare.com/", out, sizeof(out)) > 0);
  CHECK(std::string(out) == "https%3A%2F%2Fcp.cloudflare.com%2F");
  CHECK(urlEncode("abc-_.~", out, sizeof(out)) == 7 && std::string(out) == "abc-_.~");
  char tiny[8];
  CHECK(urlEncode("\xF0\x9F\x9A\x80\xF0\x9F\x9A\x80", tiny, sizeof(tiny)) == 0);   // 放不下

  // 路径
  char path[384];
  CHECK(buildDelayPath("DIRECT", "http://www.baidu.com/", 5000, path, sizeof(path)));
  CHECK(std::string(path) == "/proxies/DIRECT/delay?url=http%3A%2F%2Fwww.baidu.com%2F&timeout=5000");
  CHECK(buildDelayPath("\xF0\x9F\x9A\x80 \xE8\x8A\x82\xE7\x82\xB9\xE9\x80\x89\xE6\x8B\xA9", "https://github.com/", 5000, path, sizeof(path)));
  CHECK(std::string(path) == "/proxies/%F0%9F%9A%80%20%E8%8A%82%E7%82%B9%E9%80%89%E6%8B%A9/delay?url=https%3A%2F%2Fgithub.com%2F&timeout=5000");
  char small[20];
  CHECK(!buildDelayPath("DIRECT", "http://www.baidu.com/", 5000, small, sizeof(small)));

  printf("OK: %d checks passed\n", checks);
  return 0;
}
