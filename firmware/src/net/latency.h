#pragma once
#include <Arduino.h>

void latencyBegin();
void latencyLoop();
void latencyKick();  // 提前唤醒后台任务，立即测一轮（切回市电档时用）
