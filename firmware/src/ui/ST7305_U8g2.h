#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

// Waveshare ESP32-S3-RLCD-4.2 的 ST7305 自定义驱动。
// 官方 U8g2 内置的 ST7305 构造器与这块面板的初始化/数据格式不匹配，
// 因此使用 Waveshare 官方示例中的自定义驱动（u8x8 自定义 display/byte 回调 + 4 级灰度打包）。
class ST7305_U8g2 {
private:
  int _sck;
  int _mosi;
  int _dc;
  int _cs;
  int _rst;
  SPIClass *_spi = nullptr;
  U8G2 u8g2_wrapper;
  uint8_t *_my_buf = nullptr;

  void _cmd(uint8_t cmd);
  void _data(const uint8_t *data, size_t len);
  void _cmd_data(uint8_t cmd, const uint8_t *data, size_t len);

  static uint8_t u8x8_d_st7305_custom(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);
  static uint8_t u8x8_byte_custom(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

public:
  ST7305_U8g2(int sck = 11, int mosi = 12, int dc = 5, int cs = 40, int rst = 41);
  ~ST7305_U8g2();

  // tile_buf_height 0 表示全缓冲模式（300x400 约 15KB RAM）。
  void begin(uint8_t tile_buf_height = 0, const u8g2_cb_t *rotation = U8G2_R0);
  void reset();
  void fullInit();

  U8G2 *getU8g2() { return &u8g2_wrapper; }

  // ===== TE 同步（临时实验，可整体删除）=====
  // 初始化后调用：采样 TE 引脚判断是否在翻转、脉冲极性。不翻转则 teWait() 直接返回 false（不等待）。
  void teBegin(int tePin);
  // 等 TE 脉冲起点（面板进入 V-blank），最多等 timeoutMs。返回 true=等到；false=超时/TE 不可用。
  bool teWait(uint32_t timeoutMs);
  uint32_t teWaits = 0;     // 等到的次数
  uint32_t teTimeouts = 0;  // 超时次数
  bool teActive() const { return _teActive; }

private:
  int _tePin = -1;
  bool _teActive = false;
  bool _tePulseHigh = true;  // 脉冲极性：true = 高电平脉冲（V-blank 为高）
};
