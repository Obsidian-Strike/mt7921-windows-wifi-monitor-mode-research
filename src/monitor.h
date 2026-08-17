/*
 * monitor.h - Monitor mode control declarations
 */
#pragma once
#include "filter.h"

NDIS_STATUS MonitorEnable(PFILTER_CONTEXT ctx);
NDIS_STATUS MonitorDisable(PFILTER_CONTEXT ctx);
NDIS_STATUS MonitorSetChannel(PFILTER_CONTEXT ctx, UINT8 channel, UINT8 bw, UINT8 sco);
NDIS_STATUS MonitorSendMcuSniffer(PFILTER_CONTEXT ctx, BOOLEAN enable);
NDIS_STATUS MonitorSendMcuChannel(PFILTER_CONTEXT ctx, UINT8 ch, UINT8 band, UINT8 bw, UINT8 sco);
