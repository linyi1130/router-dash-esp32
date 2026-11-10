#pragma once
#include <Arduino.h>

#include "power_logic.h"

// 市电 / 电池两档刷新频率。各数据源每次循环通过 powerIv() 取当前周期，切换立即生效。
const power::Intervals& powerIv();

void powerBegin();
void powerLoop();  // 每个主循环调用一次：评估档位、必要时切换并打印 [power]
