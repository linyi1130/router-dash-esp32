#pragma once
// 帧缓冲差异比较的纯逻辑（不依赖 Arduino / U8g2），固件和主机端测试共用。
//
// U8g2 全缓冲布局：tileH 行 × tileW 列的 8 字节图块，每个图块行连续存放 tileW*8 字节
// （每字节是 8 个纵向像素）。本板旋转 U8G2_R1 后缓冲仍是面板原生方向（38 × 50 图块），
// 一个图块行 = 屏幕上 8 像素宽的竖条。只发变化的图块区间，未变化的不发。
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace framediff {

constexpr int kTileBytes = 8;
constexpr int kMergeGapTiles = 2;  // 同一行里相距不超过这么多个未变图块的两段合并成一次发送

// 需要发送的区间：第 ty 行，从第 tx 个图块起共 tw 个图块。
struct Run {
  uint8_t ty;
  uint8_t tx;
  uint8_t tw;
};

// 比较 prev 与 cur（大小都是 tileW*tileH*8），把变化区间写进 out。
// 返回区间个数；0 = 两帧完全相同；-1 = out 放不下（调用方应退回整屏发送）。
inline int computeDirty(const uint8_t* prev, const uint8_t* cur, int tileW, int tileH, Run* out, int cap) {
  const size_t rowBytes = static_cast<size_t>(tileW) * kTileBytes;
  int n = 0;
  for (int ty = 0; ty < tileH; ty++) {
    const uint8_t* p = prev + static_cast<size_t>(ty) * rowBytes;
    const uint8_t* c = cur + static_cast<size_t>(ty) * rowBytes;
    if (memcmp(p, c, rowBytes) == 0) continue;  // 这一行没变
    int tx = 0;
    while (tx < tileW) {
      if (memcmp(p + tx * kTileBytes, c + tx * kTileBytes, kTileBytes) == 0) {
        tx++;
        continue;
      }
      int start = tx;
      int last = tx;  // 最后一个变化的图块
      int scan = tx + 1;
      while (scan < tileW && scan - last <= kMergeGapTiles + 1) {
        if (memcmp(p + scan * kTileBytes, c + scan * kTileBytes, kTileBytes) != 0) last = scan;
        scan++;
      }
      if (n >= cap) return -1;
      out[n].ty = static_cast<uint8_t>(ty);
      out[n].tx = static_cast<uint8_t>(start);
      out[n].tw = static_cast<uint8_t>(last - start + 1);
      n++;
      tx = last + 1;
    }
  }
  return n;
}

// 累计统计，用于串口每 60 秒打印一次。
struct Stats {
  uint32_t drawn = 0;     // 实际画了、且与上一帧不同的帧数（= 发送了数据）
  uint32_t skipped = 0;   // 与上一帧完全相同、没发数据的帧数
  uint32_t partial = 0;   // 其中局部发送的帧数
  uint32_t full = 0;      // 其中整屏发送的帧数（首帧 / 区间过多）
  uint32_t bytes = 0;     // 发出的缓冲字节数（图块数 × 8）

  void reset() { *this = Stats(); }
};

inline uint32_t runBytes(const Run* runs, int n) {
  uint32_t b = 0;
  for (int i = 0; i < n; i++) b += static_cast<uint32_t>(runs[i].tw) * kTileBytes;
  return b;
}

}  // namespace framediff
