#pragma once
#include <Arduino.h>

// Waveshare ESP32-S3-RLCD-4.2
// ESP32-S3-WROOM-1-N16R8，4.2" 400x300 反射式单色 LCD（ST7305，SPI 接口，无背光）。
// https://docs.waveshare.com/ESP32-S3-RLCD-4.2
//
// 显示驱动：U8g2 的 U8G2_ST7305_300X400_F_4W_HW_SPI，U8G2_R1 旋转为横屏 400x300。

static constexpr int PIN_RLCD_SCK = 11;
static constexpr int PIN_RLCD_MOSI = 12;
static constexpr int PIN_RLCD_DC = 5;
static constexpr int PIN_RLCD_CS = 40;
static constexpr int PIN_RLCD_RST = 41;

// 按键（低电平按下）。BOOT=GPIO0，KEY=GPIO18。
static constexpr int PIN_BTN_BOOT = 0;
static constexpr int PIN_BTN_KEY = 18;

// 逻辑显示尺寸（横屏）。
static constexpr int kWidth = 400;
static constexpr int kHeight = 300;
