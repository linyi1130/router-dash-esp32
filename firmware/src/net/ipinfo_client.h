#pragma once
#include <Arduino.h>

// 网络信息：国内出口 IP（直连分流）与全球节点出口 IP（代理分流）。
void ipinfoBegin();
void ipinfoLoop();
void ipinfoKick();  // 立即刷新（切回市电档时用）
