#pragma once
#include <Arduino.h>

// 通过 UPnP IGD（WANCommonInterfaceConfig）读取路由器 WAN 口总流量。
// 不登录华硕后台，只读 SOAP（GetTotalBytesReceived/Sent），结果写入 g.wanDownSpeed / g.wanUpSpeed / g.wanValid。
void upnpBegin();
void upnpLoop();
void upnpDisconnect();
void upnpKick();  // 立即采样一次（切回市电档时用）
