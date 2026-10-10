#include "ops/mediamtx.hpp"

namespace mv
{
std::string renderMediamtxConfig(Config const& cfg)
{
    std::string yml;
    yml += "logLevel: warn\n";
    yml += "api: false\n";
    yml += "metrics: false\n";
    yml += "pprof: false\n";
    yml += "playback: false\n";
    yml += "rtsp: true\n";
    yml += "rtspAddress: 127.0.0.1:" + std::to_string(cfg.mediamtxRtspPort) + "\n";
    // TCP only: without UDP, MediaMTX binds no RTP/RTCP ports (8000/8001 in every instance).
    yml += "rtspTransports: [tcp]\n";
    yml += "rtmp: false\n";
    yml += "srt: false\n";
    yml += "moq: false\n";
    yml += "hls: true\n";
    yml += "hlsAddress: :" + std::to_string(cfg.mediamtxHlsPort) + "\n";
    yml += "hlsAllowOrigins: [\"*\"]\n";
    yml += "hlsVariant: lowLatency\n";
    yml += "hlsSegmentDuration: 1s\n";
    yml += "hlsPartDuration: 200ms\n";
    yml += "webrtc: true\n";
    yml += "webrtcAddress: :" + std::to_string(cfg.mediamtxWhepPort) + "\n";
    yml += "webrtcAllowOrigins: [\"*\"]\n";
    yml += "webrtcLocalUDPAddress: :" + std::to_string(cfg.mediamtxIcePort) + "\n";
    yml += "webrtcLocalTCPAddress: :" + std::to_string(cfg.mediamtxIcePort) + "\n";
    yml += cfg.nmosHostAddress.empty() ? "webrtcAdditionalHosts: []\n" : "webrtcAdditionalHosts: [\"" + cfg.nmosHostAddress + "\"]\n";
    yml += "webrtcICEServers2: []\n";
    yml += "pathDefaults:\n";
    yml += "  source: publisher\n";
    yml += "paths:\n";
    yml += "  all_others:\n";
    return yml;
}
} // namespace mv
