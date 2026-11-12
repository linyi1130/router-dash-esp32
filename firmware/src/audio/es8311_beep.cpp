#include "es8311_beep.h"

#include <Wire.h>
#include <math.h>

#include "board.h"
#include "driver/i2s.h"

namespace {

constexpr uint8_t kEs8311Addr = 0x18;
constexpr int kSampleRate = 16000;
constexpr int16_t kAmplitude = 25000;  // 接近满幅度，方便听清
constexpr uint8_t kDacVolume = 0xBF;   // ES8311 0xBF=0dB
constexpr int kFadeMs = 12;
constexpr int kLastFadeOutMs = 80;
constexpr int kGapMs = 24;

struct Note {
  uint16_t hz;
  uint16_t ms;
};

// 电脑上预听通过的原创短旋律：C5 D5 E5 G5 E5 D5 C5 E5 G5
constexpr Note kPhrase[] = {
    {523, 140}, {587, 140}, {659, 160}, {784, 220}, {659, 140},
    {587, 140}, {523, 160}, {659, 140}, {784, 360},
};
constexpr int kNoteCount = static_cast<int>(sizeof(kPhrase) / sizeof(kPhrase[0]));
constexpr i2s_port_t kI2sPort = I2S_NUM_0;

bool pending = false;
bool loggedFail = false;

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kEs8311Addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// 16 kHz、MCLK=256×fs、I2S 从模式、16 bit。寄存器值与常见 ES8311 从模式配置一致。
bool codecInit() {
  if (!writeReg(0x00, 0x1F)) return false;  // 复位
  delay(20);
  if (!writeReg(0x00, 0x80)) return false;  // CSM on，slave（bit6=0）

  bool ok = true;
  ok = writeReg(0x01, 0x3F) && ok;  // 时钟开启，MCLK 来自引脚
  ok = writeReg(0x02, 0x40) && ok;  // pre_div=3, pre_multi=1
  ok = writeReg(0x03, 0x10) && ok;  // 单速，ADC OSR
  ok = writeReg(0x04, 0x20) && ok;  // DAC OSR
  ok = writeReg(0x05, 0x00) && ok;  // ADC/DAC div=1
  ok = writeReg(0x06, 0x03) && ok;  // BCLK div=4
  ok = writeReg(0x07, 0x00) && ok;
  ok = writeReg(0x08, 0xFF) && ok;  // LRCK = MCLK/256
  ok = writeReg(0x09, 0x0C) && ok;  // I2S 16-bit DAC
  ok = writeReg(0x0A, 0x0C) && ok;  // I2S 16-bit ADC（本功能不用）
  ok = writeReg(0x0B, 0x00) && ok;
  ok = writeReg(0x0C, 0x00) && ok;
  ok = writeReg(0x0D, 0x01) && ok;
  ok = writeReg(0x0E, 0x02) && ok;
  ok = writeReg(0x10, 0x1F) && ok;
  ok = writeReg(0x11, 0x7F) && ok;
  ok = writeReg(0x12, 0x00) && ok;
  ok = writeReg(0x14, 0x1A) && ok;
  ok = writeReg(0x15, 0x40) && ok;
  ok = writeReg(0x16, 0x24) && ok;
  ok = writeReg(0x17, 0xBF) && ok;
  ok = writeReg(0x32, kDacVolume) && ok;
  ok = writeReg(0x37, 0x08) && ok;
  ok = writeReg(0x44, 0x50) && ok;
  ok = writeReg(0x45, 0x00) && ok;
  return ok;
}

void codecStandby() {
  writeReg(0x32, 0x00);
  writeReg(0x00, 0x1F);
}

bool i2sStart() {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = true;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = kSampleRate * 256;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_I2S_MCLK;
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_LRCLK;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(kI2sPort, &cfg, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(kI2sPort, &pins) != ESP_OK) {
    i2s_driver_uninstall(kI2sPort);
    return false;
  }
  return true;
}

void writeSilence(int ms) {
  const int frames = kSampleRate * ms / 1000;
  int16_t buf[256] = {};
  int written = 0;
  while (written < frames) {
    int n = frames - written;
    if (n > 128) n = 128;
    size_t bytes = 0;
    i2s_write(kI2sPort, buf, static_cast<size_t>(n) * 4, &bytes, portMAX_DELAY);
    written += n;
  }
}

void playNote(uint16_t hz, uint16_t ms, int fadeInMs, int fadeOutMs) {
  const int frames = kSampleRate * ms / 1000;
  const int fadeIn = kSampleRate * fadeInMs / 1000;
  const int fadeOut = kSampleRate * fadeOutMs / 1000;
  int16_t buf[256];  // 128 帧立体声
  int written = 0;
  const float freq = static_cast<float>(hz);
  while (written < frames) {
    int n = frames - written;
    if (n > 128) n = 128;
    for (int i = 0; i < n; i++) {
      const int idx = written + i;
      int amp = kAmplitude;
      if (idx < fadeIn && fadeIn > 0) amp = kAmplitude * idx / fadeIn;
      const int remain = frames - idx;
      if (remain <= fadeOut && fadeOut > 0) {
        const int outAmp = kAmplitude * remain / fadeOut;
        if (outAmp < amp) amp = outAmp;
      }
      const float s = sinf(2.0f * 3.14159265f * freq * static_cast<float>(idx) /
                           static_cast<float>(kSampleRate));
      const float h = sinf(4.0f * 3.14159265f * freq * static_cast<float>(idx) /
                           static_cast<float>(kSampleRate));
      const int16_t v = static_cast<int16_t>((s * 0.86f + h * 0.14f) * static_cast<float>(amp));
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    size_t bytes = 0;
    i2s_write(kI2sPort, buf, static_cast<size_t>(n) * 4, &bytes, portMAX_DELAY);
    written += n;
  }
}

void playTone() {
  for (int i = 0; i < kNoteCount; i++) {
    const int fadeOut = (i == kNoteCount - 1) ? kLastFadeOutMs : kFadeMs;
    playNote(kPhrase[i].hz, kPhrase[i].ms, kFadeMs, fadeOut);
    if (i < kNoteCount - 1) writeSilence(kGapMs);
  }
}

void playOnce() {
  if (!i2sStart()) {
    if (!loggedFail) {
      Serial.println("[beep] I2S 初始化失败");
      loggedFail = true;
    }
    return;
  }
  if (!codecInit()) {
    if (!loggedFail) {
      Serial.println("[beep] ES8311 无应答，跳过提示音");
      loggedFail = true;
    }
    digitalWrite(PIN_PA_EN, LOW);
    i2s_driver_uninstall(kI2sPort);
    return;
  }
  digitalWrite(PIN_PA_EN, HIGH);
  delay(15);
  playTone();
  Serial.println("[beep] 已播放提示音");
  i2s_zero_dma_buffer(kI2sPort);
  delay(20);
  digitalWrite(PIN_PA_EN, LOW);
  codecStandby();
  i2s_driver_uninstall(kI2sPort);
}

}  // namespace

void beepBegin() {
  pinMode(PIN_PA_EN, OUTPUT);
  digitalWrite(PIN_PA_EN, LOW);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);
}

void beepRequest() { pending = true; }

void beepLoop() {
  if (!pending) return;
  pending = false;
  playOnce();
}
