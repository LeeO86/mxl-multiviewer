#pragma once

#include <cstdint>
#include <string>

namespace mv
{
struct Debounce
{
    bool raw = false;
    bool active = false;
    std::int64_t sinceMs = 0;

    // Returns true when `active` changes.
    bool update(bool rawNow, std::int64_t nowMs, int assertMs, int clearMs);
};

struct AlarmSet
{
    Debounce noSignal;
    Debounce black;
    Debounce freeze;
    Debounce silence;
    Debounce clip;
    Debounce formatMismatch;
    std::uint64_t lastHash = 0;
    bool haveHash = false;
};

struct AlarmNames
{
    static constexpr char const* noSignal = "no_signal";
    static constexpr char const* black = "black";
    static constexpr char const* freeze = "freeze";
    static constexpr char const* silence = "silence";
    static constexpr char const* clip = "clip";
    static constexpr char const* formatMismatch = "format_mismatch";
};
} // namespace mv
