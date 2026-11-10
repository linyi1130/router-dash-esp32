// g++ -std=c++17 -Wall -Wextra -I../src/ui test_frame_diff.cpp -o /tmp/test_frame_diff
#include <stdio.h>
#include <string.h>
#include <vector>

#include "frame_diff.h"

static int checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

constexpr int W = 38, H = 50;
constexpr size_t SZ = W * H * 8;

static void touch(std::vector<uint8_t>& b, int tx, int ty, int byte = 0) { b[(ty * W + tx) * 8 + byte] ^= 0x10; }

int main() {
  using namespace framediff;
  std::vector<uint8_t> prev(SZ, 0), cur(SZ, 0);
  Run runs[200];

  // 相同
  CHECK(computeDirty(prev.data(), cur.data(), W, H, runs, 200) == 0);

  // 一个像素变化 -> 一个图块
  touch(cur, 5, 7, 3);
  int n = computeDirty(prev.data(), cur.data(), W, H, runs, 200);
  CHECK(n == 1 && runs[0].ty == 7 && runs[0].tx == 5 && runs[0].tw == 1);
  CHECK(runBytes(runs, n) == 8);

  // 同一行相邻变化 -> 合并为连续一段
  touch(cur, 6, 7);
  touch(cur, 7, 7, 7);
  n = computeDirty(prev.data(), cur.data(), W, H, runs, 200);
  CHECK(n == 1 && runs[0].tx == 5 && runs[0].tw == 3);

  // 间隔 <= kMergeGapTiles 的两段合并（含中间未变图块）
  prev.assign(SZ, 0); cur.assign(SZ, 0);
  touch(cur, 2, 10); touch(cur, 2 + 1 + kMergeGapTiles, 10);
  n = computeDirty(prev.data(), cur.data(), W, H, runs, 200);
  CHECK(n == 1 && runs[0].tx == 2 && runs[0].tw == 2 + kMergeGapTiles);

  // 间隔更大 -> 两段
  cur.assign(SZ, 0);
  touch(cur, 2, 10); touch(cur, 2 + 2 + kMergeGapTiles, 10);
  n = computeDirty(prev.data(), cur.data(), W, H, runs, 200);
  CHECK(n == 2 && runs[0].tx == 2 && runs[0].tw == 1 && runs[1].tx == 2 + 2 + kMergeGapTiles && runs[1].tw == 1);

  // 不同行、边界（第 0 行、最后一行、最后一列）
  cur.assign(SZ, 0);
  touch(cur, 0, 0); touch(cur, W - 1, H - 1); touch(cur, W - 1, 20);
  n = computeDirty(prev.data(), cur.data(), W, H, runs, 200);
  CHECK(n == 3);
  CHECK(runs[0].ty == 0 && runs[0].tx == 0 && runs[0].tw == 1);
  CHECK(runs[1].ty == 20 && runs[1].tx == W - 1 && runs[1].tw == 1);
  CHECK(runs[2].ty == H - 1 && runs[2].tx == W - 1 && runs[2].tw == 1);
  for (int i = 0; i < n; i++) CHECK(runs[i].tx + runs[i].tw <= W);

  // 全部变化 -> 每行一段、整行；容量不足 -> -1
  memset(cur.data(), 0xFF, SZ);
  n = computeDirty(prev.data(), cur.data(), W, H, runs, 200);
  CHECK(n == H && runs[0].tx == 0 && runs[0].tw == W && runBytes(runs, n) == SZ);
  CHECK(computeDirty(prev.data(), cur.data(), W, H, runs, 10) == -1);

  // 恰好放满不报溢出；多一个才溢出
  cur.assign(SZ, 0);
  for (int y = 0; y < 4; y++) touch(cur, 3, y);
  CHECK(computeDirty(prev.data(), cur.data(), W, H, runs, 4) == 4);
  touch(cur, 3, 4);
  CHECK(computeDirty(prev.data(), cur.data(), W, H, runs, 4) == -1);

  // 应用区间后 prev 与 cur 一致（模拟 presentFrame：只拷贝/发送区间 == 整帧一致）
  cur.assign(SZ, 0);
  for (int i = 0; i < 300; i++) cur[(i * 37) % SZ] ^= (uint8_t)(i * 13 + 1);
  Run big[H * 20];
  n = computeDirty(prev.data(), cur.data(), W, H, big, H * 20);
  CHECK(n > 0);
  std::vector<uint8_t> panel = prev;
  for (int i = 0; i < n; i++)
    memcpy(&panel[(big[i].ty * W + big[i].tx) * 8], &cur[(big[i].ty * W + big[i].tx) * 8], big[i].tw * 8);
  CHECK(panel == cur);  // 区间覆盖了所有变化

  // Stats
  Stats s; s.drawn = 3; s.bytes = 99; s.reset();
  CHECK(s.drawn == 0 && s.skipped == 0 && s.partial == 0 && s.full == 0 && s.bytes == 0);

  printf("test_frame_diff: all %d checks passed\n", checks);
  return 0;
}
