#pragma once

// 通过板载 ES8311 + SPK 座播放一声短提示音。
// beepBegin() 只把功放拉低并初始化 I2C；真正上电在第一次播放时。
void beepBegin();
void beepRequest();  // 在本圈 loop 里播放，避免卡在 HTTP 解析里
void beepLoop();
