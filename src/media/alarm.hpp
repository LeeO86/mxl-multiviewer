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

// Freeze (§6.3): the picture hash has not changed for MV_FREEZE_MS, counted from the
// last change. A source that repeats grains (25p in 50p, a browser that paints slower
// than the output rate) changes only every other grain; comparing consecutive grains
// raised the alarm on every repeat and a still moment kept it up for good.
struct FreezeDetector
{
    std::uint64_t lastHash = 0;
    std::int64_t changedMs = 0;
    bool have = false;

    // True while the picture has been unchanged for at least `freezeMs`.
    bool update(std::uint64_t hash, std::int64_t nowMs, int freezeMs);
    void reset()
    {
        have = false;
    }
};

struct AlarmSet
{
    Debounce noSignal;
    Debounce black;
    Debounce freeze;
    Debounce silence;
    Debounce clip;
    Debounce formatMismatch;
    FreezeDetector picture;
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
