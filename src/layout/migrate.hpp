#pragma once

#include <string>
#include <vector>

#include "layout/model.hpp"

namespace mv
{
// The built-in presets of 1.1.x (preset revision 1: no audio bars), frozen for the
// migration. Do not change them when the current presets change.
std::vector<Layout> legacyPresets(int maxInputs);

// Every field equal; rect values within 1e-5, because layout files keep six significant digits.
bool sameLayout(Layout const& a, Layout const& b);

// A book below kPresetRevision: each layout that is an unedited 1.1.x preset (equal to
// legacyPresets() of the same name for any input count) becomes today's preset of that name.
// Edited layouts stay. Returns the replaced names; the book is at kPresetRevision afterwards.
std::vector<std::string> migratePresets(LayoutBook& book, int maxInputs);
} // namespace mv
