#pragma once
#include <Arduino.h>

// 读取电池电压（GPIO4 / ADC1_CH3，1/3 分压）并换算成百分比，写入 g.battMv / g.battPercent / g.battCharging / g.battValid。
void battBegin();
void battLoop();
void battKick();  // 立即触发一次采样（切回市电档时用）

#include "battery_logic.h"
// 当前充电推断：Unknown（开机/数据不足）/ Charging / Discharging。
batt::ChargeState battChargeState();
