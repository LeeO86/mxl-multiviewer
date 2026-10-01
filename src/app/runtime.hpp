#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "config/config.hpp"

namespace mv
{
struct LegView
{
    bool enable = false;
    std::string domainId;
    std::string flowId;
    std::string senderId;
    std::string state = "not_routed";
    std::string reason;
    std::string format;
    std::string label;
    int width = 0;
    int height = 0;
    int rateNum = 0;
    int rateDen = 1;
    int channels = 0;
    bool interlaced = false;
    std::uint64_t grains = 0;
    std::uint64_t late = 0;
    std::uint64_t resyncs = 0;
    double latencyMs = 0;
};

struct InputView
{
    int index = 1;
    LegView video;
    LegView audio;
    std::array<double, 16> ppmDbfs{};
    std::array<double, 16> rmsDbfs{};
    std::array<bool, 16> clip{};
    bool alarmNoSignal = false;
    bool alarmBlack = false;
    bool alarmFreeze = false;
    bool alarmSilence = false;
    bool alarmClip = false;
    bool alarmFormat = false;
    std::string tslText;
    int tally = 0;
};

struct OutputView
{
    int index = 1;
    std::string format;
    std::string layout;
    std::string backend = "cpu";
    std::string videoFlowId;
    std::string audioFlowId;
    std::string domainId;
    std::uint64_t frames = 0;
    std::uint64_t late = 0;
    std::uint64_t missed = 0;
    double composeMs = 0;
    int audioFollow = 1;
    int audioChannels = 2;
};

class RuntimeModel
{
public:
    explicit RuntimeModel(Config const& config);

    void setInput(InputView view);
    void setTally(int input, std::string text, int tally);
    void addLate(int input);
    void setOutput(OutputView view);
    void setPreview(std::string jpeg);
    void setNmosUp(bool up);
    void setGpu(bool compiled, int devices);
    void touch();

    [[nodiscard]] std::vector<InputView> inputs() const;
    [[nodiscard]] std::vector<OutputView> outputs() const;
    [[nodiscard]] OutputView output(int index) const;
    [[nodiscard]] std::string preview() const;
    [[nodiscard]] bool nmosUp() const;
    [[nodiscard]] bool cudaCompiled() const;
    [[nodiscard]] int cudaDevices() const;
    [[nodiscard]] std::uint64_t heartbeatNs() const;
    void setHeadLayout(int index, std::string layout);
    void setHeadFormat(int index, VideoFormat format);
    void setHeadAudio(int index, int follow, int channels);
    [[nodiscard]] std::string headLayout(int index) const;
    [[nodiscard]] VideoFormat headFormat(int index) const;
    [[nodiscard]] int headAudioFollow(int index) const;
    [[nodiscard]] int headAudioChannels(int index) const;

private:
    mutable std::mutex mutex_;
    std::vector<InputView> inputs_;
    std::vector<OutputView> outputs_;
    std::vector<VideoFormat> formats_;
    std::string preview_;
    bool nmosUp_ = false;
    bool cudaCompiled_ = false;
    int cudaDevices_ = 0;
    std::uint64_t heartbeatNs_ = 0;
};
} // namespace mv
