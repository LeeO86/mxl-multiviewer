#include "media/alarm.hpp"

namespace mv
{
bool Debounce::update(bool rawNow, std::int64_t nowMs, int assertMs, int clearMs)
{
    if (rawNow != raw)
    {
        raw = rawNow;
        sinceMs = nowMs;
    }
    bool const before = active;
    if (!active && raw && (nowMs - sinceMs) >= assertMs)
    {
        active = true;
    }
    if (active && !raw && (nowMs - sinceMs) >= clearMs)
    {
        active = false;
    }
    return active != before;
}

bool FreezeDetector::update(std::uint64_t hash, std::int64_t nowMs, int freezeMs)
{
    if (!have || hash != lastHash)
    {
        have = true;
        lastHash = hash;
        changedMs = nowMs;
    }
    return nowMs - changedMs >= freezeMs;
}
} // namespace mv
