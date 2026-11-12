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
static constexpr int PIN_RLCD_TE = 6;   // ST7305 TE（撕裂效应）输出，原理图网络 LCD_TE

// 按键（低电平按下）。BOOT=GPIO0，KEY=GPIO18。
static constexpr int PIN_BTN_BOOT = 0;
static constexpr int PIN_BTN_KEY = 18;

// 音频：板载 ES8311 DAC（I2C 0x18）+ NS4150B 功放，喇叭接 MX1.25 SPK 座。
// 不是 GPIO 蜂鸣器。PA 高电平使能，平时拉低，避免底噪和空耗。
// GPIO45 / GPIO46 是 ESP32-S3 的 strapping 脚，本板原理图已按此使用。
static constexpr int PIN_I2C_SDA = 13;
static constexpr int PIN_I2C_SCL = 14;
static constexpr int PIN_I2S_MCLK = 16;
static constexpr int PIN_I2S_BCLK = 9;
static constexpr int PIN_I2S_LRCLK = 45;
static constexpr int PIN_I2S_DOUT = 8;
static constexpr int PIN_PA_EN = 46;

// 逻辑显示尺寸（横屏）。
static constexpr int kWidth = 400;
static constexpr int kHeight = 300;
