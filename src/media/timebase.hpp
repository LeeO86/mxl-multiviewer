#pragma once

#include <cstdint>
#include <string>

namespace mv
{
// Same 128-bit rounding as dmf-mxl/mxl lib/internal IndexConversion.hpp at 218ddaa.
// Index 0 is the SMPTE ST 2059 epoch. A zero numerator or denominator yields UINT64_MAX.
std::uint64_t timestampToIndex(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t timestampNs);
std::uint64_t indexToTimestamp(std::int64_t rateNumerator, std::int64_t rateDenominator, std::uint64_t index);

// CLOCK_TAI when the kernel exposes it, otherwise CLOCK_REALTIME.
std::uint64_t taiNowNs();

// `25`, `50`, `30000/1001`, `2997` (→ 30000/1001), `2398`, `5994`.
bool parseRateToken(std::string const& text, int& numerator, int& denominator);

// HH:MM:SS:FF from the TAI timestamp's index at this rate. The frame number is the
// index delta from the start of that TAI second.
std::string formatTimecode(std::uint64_t timestampNs, int numerator, int denominator);
} // namespace mv
