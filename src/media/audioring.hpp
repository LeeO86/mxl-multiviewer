#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mv
{
// Recent samples of one input's audio for audio-follow. The audio reader appends what it
// read; the composer copies the samples of one output frame. Both hold the lock only for
// that copy, so nothing larger than the new samples moves per read.
class AudioRing
{
public:
    explicit AudioRing(std::size_t capacity = 24000);

    // Drops every sample (route change, no signal).
    void clear();

    // Appends `count` samples starting at sample index `first`, one pointer per channel.
    // A gap or a jump back starts the ring over at `first`; another channel count resets it.
    void push(std::uint64_t first, std::vector<float const*> const& channels, std::size_t count);

    // Copies channel `channel`'s samples [first, first + count) into `dst`. Samples the ring
    // does not hold leave `dst` as it is (the caller fills silence first).
    void copy(int channel, std::uint64_t first, std::size_t count, float* dst) const;

    [[nodiscard]] int channels() const;

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    int channels_ = 0;
    std::vector<float> samples_; // channel-major, capacity_ per channel
    std::uint64_t begin_ = 0; // oldest index held
    std::uint64_t end_ = 0; // one past the newest index held
};
} // namespace mv
