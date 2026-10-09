#include "app/runtime.hpp"

#include "media/timebase.hpp"

namespace mv
{
RuntimeModel::RuntimeModel(Config const& config)
{
    inputs_.resize(static_cast<std::size_t>(config.maxInputs));
    outputs_.resize(static_cast<std::size_t>(config.outputs));
    formats_.resize(static_cast<std::size_t>(config.outputs));
    previews_.resize(static_cast<std::size_t>(config.outputs));
    for (int i = 0; i < config.maxInputs; ++i)
    {
        inputs_[static_cast<std::size_t>(i)].index = i + 1;
        inputs_[static_cast<std::size_t>(i)].ppmDbfs.fill(-120);
        inputs_[static_cast<std::size_t>(i)].holdDbfs.fill(-120);
        inputs_[static_cast<std::size_t>(i)].rmsDbfs.fill(-120);
    }
    for (int i = 0; i < config.outputs; ++i)
    {
        outputs_[static_cast<std::size_t>(i)].index = i + 1;
        outputs_[static_cast<std::size_t>(i)].format = config.heads[static_cast<std::size_t>(i)].format.token();
        outputs_[static_cast<std::size_t>(i)].layout = config.heads[static_cast<std::size_t>(i)].layout;
        outputs_[static_cast<std::size_t>(i)].audioFollow = config.heads[static_cast<std::size_t>(i)].audioFollow;
        outputs_[static_cast<std::size_t>(i)].audioChannels = config.heads[static_cast<std::size_t>(i)].audioChannels;
        outputs_[static_cast<std::size_t>(i)].backend = config.backend == "cuda" ? "cuda" : "cpu";
        formats_[static_cast<std::size_t>(i)] = config.heads[static_cast<std::size_t>(i)].format;
    }
}

void RuntimeModel::setInputVideo(InputView const& view)
{
    std::lock_guard lock{mutex_};
    if (view.index >= 1 && view.index <= static_cast<int>(inputs_.size()))
    {
        auto& slot = inputs_[static_cast<std::size_t>(view.index - 1)];
        // `late` is counted by the composer (addLate).
        auto const late = slot.video.late;
        slot.video = view.video;
        slot.video.late = late;
        slot.alarmNoSignal = view.alarmNoSignal;
        slot.alarmBlack = view.alarmBlack;
        slot.alarmFreeze = view.alarmFreeze;
        slot.alarmFormat = view.alarmFormat;
        for (auto const index : {kAlarmNoSignal, kAlarmBlack, kAlarmFreeze, kAlarmFormat})
        {
            slot.alarmSinceMs[index] = view.alarmSinceMs[index];
        }
    }
}

void RuntimeModel::setInputAudio(InputView const& view)
{
    std::lock_guard lock{mutex_};
    if (view.index >= 1 && view.index <= static_cast<int>(inputs_.size()))
    {
        auto& slot = inputs_[static_cast<std::size_t>(view.index - 1)];
        slot.audio = view.audio;
        slot.ppmDbfs = view.ppmDbfs;
        slot.holdDbfs = view.holdDbfs;
        slot.rmsDbfs = view.rmsDbfs;
        slot.clip = view.clip;
        slot.alarmSilence = view.alarmSilence;
        slot.alarmClip = view.alarmClip;
        for (auto const index : {kAlarmSilence, kAlarmClip})
        {
            slot.alarmSinceMs[index] = view.alarmSinceMs[index];
        }
    }
}

void RuntimeModel::setTally(int input, TallyUpdate const& update)
{
    std::lock_guard lock{mutex_};
    if (input >= 1 && input <= static_cast<int>(inputs_.size()))
    {
        auto& slot = inputs_[static_cast<std::size_t>(input - 1)];
        slot.tslText = update.textValue;
        slot.tally = effectiveTally(update);
        slot.tslLh = update.lh;
        slot.tslRh = update.rh;
        slot.tslTextTally = update.text;
    }
}

void RuntimeModel::addLate(int input)
{
    std::lock_guard lock{mutex_};
    if (input >= 1 && input <= static_cast<int>(inputs_.size()))
    {
        ++inputs_[static_cast<std::size_t>(input - 1)].video.late;
    }
}

void RuntimeModel::setOutput(OutputView view)
{
    std::lock_guard lock{mutex_};
    if (view.index >= 1 && view.index <= static_cast<int>(outputs_.size()))
    {
        // Layout and audio-follow belong to the API (setHeadLayout, setHeadAudio). The
        // composer's copy may be a frame old; writing it back lost an activation that
        // arrived during that frame.
        auto& slot = outputs_[static_cast<std::size_t>(view.index - 1)];
        view.layout = slot.layout;
        view.audioFollow = slot.audioFollow;
        view.audioChannels = slot.audioChannels;
        slot = std::move(view);
    }
}

void RuntimeModel::setPreview(int head, std::string jpeg)
{
    std::lock_guard lock{mutex_};
    if (head >= 1 && head <= static_cast<int>(previews_.size()))
    {
        previews_[static_cast<std::size_t>(head - 1)] = std::move(jpeg);
    }
}

void RuntimeModel::setNmosUp(bool up)
{
    std::lock_guard lock{mutex_};
    nmosUp_ = up;
}

void RuntimeModel::setGpu(bool compiled, int devices)
{
    std::lock_guard lock{mutex_};
    cudaCompiled_ = compiled;
    cudaDevices_ = devices;
}

void RuntimeModel::touch()
{
    std::lock_guard lock{mutex_};
    heartbeatNs_ = taiNowNs();
}

std::vector<InputView> RuntimeModel::inputs() const
{
    std::lock_guard lock{mutex_};
    return inputs_;
}

std::vector<OutputView> RuntimeModel::outputs() const
{
    std::lock_guard lock{mutex_};
    return outputs_;
}

OutputView RuntimeModel::output(int index) const
{
    std::lock_guard lock{mutex_};
    if (index >= 1 && index <= static_cast<int>(outputs_.size()))
    {
        return outputs_[static_cast<std::size_t>(index - 1)];
    }
    return {};
}

std::string RuntimeModel::preview(int head) const
{
    std::lock_guard lock{mutex_};
    if (head >= 1 && head <= static_cast<int>(previews_.size()))
    {
        return previews_[static_cast<std::size_t>(head - 1)];
    }
    return {};
}

bool RuntimeModel::nmosUp() const
{
    std::lock_guard lock{mutex_};
    return nmosUp_;
}

bool RuntimeModel::cudaCompiled() const
{
    std::lock_guard lock{mutex_};
    return cudaCompiled_;
}

int RuntimeModel::cudaDevices() const
{
    std::lock_guard lock{mutex_};
    return cudaDevices_;
}

std::uint64_t RuntimeModel::heartbeatNs() const
{
    std::lock_guard lock{mutex_};
    return heartbeatNs_;
}

void RuntimeModel::setHeadLayout(int index, std::string layout)
{
    std::lock_guard lock{mutex_};
    if (index >= 1 && index <= static_cast<int>(outputs_.size()))
    {
        outputs_[static_cast<std::size_t>(index - 1)].layout = std::move(layout);
    }
}

void RuntimeModel::setHeadFormat(int index, VideoFormat format)
{
    std::lock_guard lock{mutex_};
    if (index >= 1 && index <= static_cast<int>(outputs_.size()))
    {
        formats_[static_cast<std::size_t>(index - 1)] = format;
        outputs_[static_cast<std::size_t>(index - 1)].format = format.token();
    }
}

void RuntimeModel::setHeadAudio(int index, int follow, int channels)
{
    std::lock_guard lock{mutex_};
    if (index >= 1 && index <= static_cast<int>(outputs_.size()))
    {
        outputs_[static_cast<std::size_t>(index - 1)].audioFollow = follow;
        outputs_[static_cast<std::size_t>(index - 1)].audioChannels = channels;
    }
}

std::string RuntimeModel::headLayout(int index) const
{
    std::lock_guard lock{mutex_};
    if (index >= 1 && index <= static_cast<int>(outputs_.size()))
    {
        return outputs_[static_cast<std::size_t>(index - 1)].layout;
    }
    return "2x2";
}

VideoFormat RuntimeModel::headFormat(int index) const
{
    std::lock_guard lock{mutex_};
    if (index >= 1 && index <= static_cast<int>(formats_.size()))
    {
        return formats_[static_cast<std::size_t>(index - 1)];
    }
    return {};
}

int RuntimeModel::headAudioFollow(int index) const
{
    return output(index).audioFollow;
}

int RuntimeModel::headAudioChannels(int index) const
{
    return output(index).audioChannels;
}
} // namespace mv
