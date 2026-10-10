#pragma once

#include "config/config.hpp"

#include <string>

namespace mv
{
// The config of the built-in MediaMTX (§8.4, own mode): RTSP ingest on 127.0.0.1:MEDIAMTX_RTSP_PORT
// (TCP only), WHEP on MEDIAMTX_WHEP_PORT with ICE on MEDIAMTX_ICE_UDP_PORT (UDP and TCP) and
// NMOS_HOST_ADDRESS as the ICE host, low-latency HLS on MEDIAMTX_HLS_PORT; no API, metrics, RTMP,
// SRT or MoQ. Adapted from mxl-webrtc-monitor 1.3.0 src/ops/mediamtx.cpp (MIT, Copyright (c) 2026
// Adrian Hilber).
std::string renderMediamtxConfig(Config const& cfg);
} // namespace mv
