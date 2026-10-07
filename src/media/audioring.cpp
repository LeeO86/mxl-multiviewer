#include "media/audioring.hpp"

#include <algorithm>
#include <cstring>

namespace mv
{
AudioRing::AudioRing(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity))
{
}

void AudioRing::clear()
{
    std::lock_guard lock{mutex_};
    begin_ = end_ = 0;
}

void AudioRing::push(std::uint64_t first, std::vector<float const*> const& channels, std::size_t count)
{
    if (channels.empty() || count == 0)
    {
        return;
    }
    std::lock_guard lock{mutex_};
    int const n = static_cast<int>(channels.size());
    if (n != channels_)
    {
        channels_ = n;
        samples_.assign(capacity_ * channels.size(), 0.f);
        begin_ = end_ = first;
    }
    if (first != end_)
    {
        begin_ = end_ = first;
    }
    // Only the newest `capacity_` samples of a long write fit.
    std::size_t const skip = count > capacity_ ? count - capacity_ : 0;
    for (int ch = 0; ch < n; ++ch)
    {
        float const* src = channels[static_cast<std::size_t>(ch)];
        if (src == nullptr)
        {
            continue;
        }
        float* base = samples_.data() + static_cast<std::size_t>(ch) * capacity_;
        std::size_t done = skip;
        while (done < count)
        {
            std::size_t const pos = static_cast<std::size_t>((first + done) % capacity_);
            std::size_t const run = std::min(count - done, capacity_ - pos);
            std::memcpy(base + pos, src + done, run * sizeof(float));
            done += run;
        }
    }
    end_ = first + count;
    begin_ = std::max(begin_, end_ > capacity_ ? end_ - capacity_ : 0);
}

void AudioRing::copy(int channel, std::uint64_t first, std::size_t count, float* dst) const
{
    std::lock_guard lock{mutex_};
    if (channel < 0 || channel >= channels_ || dst == nullptr)
    {
        return;
    }
    std::uint64_t const from = std::max(first, begin_);
    std::uint64_t const to = std::min(first + count, end_);
    float const* base = samples_.data() + static_cast<std::size_t>(channel) * capacity_;
    for (std::uint64_t index = from; index < to;)
    {
        std::size_t const pos = static_cast<std::size_t>(index % capacity_);
        std::size_t const run = static_cast<std::size_t>(std::min<std::uint64_t>(to - index, capacity_ - pos));
        std::memcpy(dst + (index - first), base + pos, run * sizeof(float));
        index += run;
    }
}

int AudioRing::channels() const
{
    std::lock_guard lock{mutex_};
    return channels_;
}
} // namespace mv
