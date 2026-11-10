#pragma once
#include <Arduino.h>

void clashBegin();
void clashLoop();
void clashDisconnect();
void clashKick();  // 立即刷新连接数据（切回市电档时用）

