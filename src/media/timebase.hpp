#pragma once

#include <cstdint>

namespace mv
{
// Same 128-bit rounding as dmf-mxl/mxl lib/internal IndexConversion.hpp at 218ddaa.
// Index 0 is the SMPTE ST 2059 epoch. A zero numerator or denominator yields UINT64_MAX.
std::uint64_t timestampToIndex(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t timestampNs);
std::uint64_t indexToTimestamp(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t index);

// CLOCK_TAI when the kernel exposes it, otherwise CLOCK_REALTIME.
std::uint64_t taiNowNs();
} // namespace mv
