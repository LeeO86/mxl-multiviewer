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

// The process's local time zone (clock tiles with clock_zone local, §6.2): TZ, which
// MV_TIMEZONE sets at start, else the zone /etc/localtime links to, else UTC. Empty when
// /etc/localtime is a copy whose name is unknown.
std::string localZoneName();
// Offset of local time from UTC now, in seconds.
long utcOffsetSeconds();

// HH:MM:SS:FF from the TAI timestamp's index at this rate. The frame number is the
// index delta from the start of that TAI second.
std::string formatTimecode(std::uint64_t timestampNs, int numerator, int denominator);
} // namespace mv
