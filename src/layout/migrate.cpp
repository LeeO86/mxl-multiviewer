#include "layout/migrate.hpp"

#include <algorithm>
#include <cmath>

namespace mv
{
namespace
{
// A 1.1.x tile with every field spelled out, so a later change of the Tile defaults does
// not change what the migration compares against.
Tile legacyTile()
{
    Tile tile;
    tile.content = TileContent::Input;
    tile.input = 1;
    tile.z = 0;
    tile.scale = ScaleMode::Fit;
    tile.umd = true;
    tile.umdSource = UmdSource::Is04;
    tile.umdText.clear();
    tile.umdPosition = UmdPosition::BottomInside;
    tile.umdFont = 28;
    tile.umdBg = "#000000c0";
    tile.tallyBorder = true;
    tile.tallyLamp = true;
    tile.audioBars = false;
    tile.audioBarRms = false;
    tile.audioBarChannels = 2;
    tile.audioBarFirst = 0;
    tile.audioBarPosition = BarsPosition::Right;
    tile.zoneGreen = -18;
    tile.zoneAmber = -9;
    tile.formatLabel = true;
    tile.latency = false;
    tile.safeArea = false;
    tile.centre = false;
    tile.aspectMarkers.clear();
    tile.clockStyle = ClockStyle::Digital;
    tile.clockZone = ClockZone::Utc;
    tile.timecodeRate.clear();
    tile.labelText.clear();
    return tile;
}

Layout legacyLayout(std::string name)
{
    Layout layout;
    layout.version = 1;
    layout.name = std::move(name);
    layout.background = "#101010";
    return layout;
}

// The 1.1.3 preset geometry (src/layout/model.cpp at v1.1.3).
Tile legacyGridTile(int index, int cols, int rows)
{
    int const col = index % cols;
    int const row = index / cols;
    Tile tile = legacyTile();
    tile.id = "t" + std::to_string(index + 1);
    tile.input = index + 1;
    tile.z = index;
    tile.rect = {static_cast<double>(col) / cols, static_cast<double>(row) / rows, 1.0 / cols, 1.0 / rows};
    return tile;
}

Layout legacyGrid(std::string name, int cols, int rows, int maxInputs)
{
    Layout layout = legacyLayout(std::move(name));
    int const count = std::min(cols * rows, maxInputs);
    for (int i = 0; i < count; ++i)
    {
        layout.tiles.push_back(legacyGridTile(i, cols, rows));
    }
    return layout;
}

bool near(double a, double b)
{
    return std::fabs(a - b) <= 1e-5;
}
} // namespace

std::vector<Layout> legacyPresets(int maxInputs)
{
    std::vector<Layout> layouts;
    layouts.push_back(legacyGrid("1", 1, 1, maxInputs));
    layouts.push_back(legacyGrid("2x2", 2, 2, maxInputs));
    layouts.push_back(legacyGrid("3x3", 3, 3, maxInputs));
    layouts.push_back(legacyGrid("4x4", 4, 4, maxInputs));
    layouts.push_back(legacyGrid("5x5", 5, 5, maxInputs));
    if (maxInputs < 1)
    {
        return layouts;
    }
    Layout twoPlusEight = legacyLayout("2+8");
    for (int i = 0; i < std::min(2, maxInputs); ++i)
    {
        Tile tile = legacyTile();
        tile.id = "t" + std::to_string(i + 1);
        tile.input = i + 1;
        tile.z = i;
        tile.rect = {0, i * 0.5, 0.5, 0.5};
        twoPlusEight.tiles.push_back(tile);
    }
    for (int i = 0; i < 8 && (i + 2) < maxInputs; ++i)
    {
        Tile tile = legacyTile();
        tile.id = "t" + std::to_string(i + 3);
        tile.input = i + 3;
        tile.z = i + 2;
        tile.rect = {0.5 + (i % 2) * 0.25, (i / 2) * 0.25, 0.25, 0.25};
        twoPlusEight.tiles.push_back(tile);
    }
    layouts.push_back(twoPlusEight);

    Layout onePlusFive = legacyLayout("1+5");
    Tile main = legacyTile();
    main.id = "t1";
    main.input = 1;
    main.rect = {0, 0, 2.0 / 3.0, 1};
    onePlusFive.tiles.push_back(main);
    for (int i = 0; i < 5 && (i + 1) < maxInputs; ++i)
    {
        Tile tile = legacyTile();
        tile.id = "t" + std::to_string(i + 2);
        tile.input = i + 2;
        tile.z = i + 1;
        tile.rect = {2.0 / 3.0, i / 5.0, 1.0 / 3.0, 0.2};
        onePlusFive.tiles.push_back(tile);
    }
    layouts.push_back(onePlusFive);

    Layout onePlusSeven = legacyLayout("1+7");
    Tile big = main;
    big.rect = {0, 0, 0.75, 0.75};
    onePlusSeven.tiles.push_back(big);
    int next = 2;
    for (int row = 0; row < 4 && next <= maxInputs; ++row, ++next)
    {
        Tile tile = legacyTile();
        tile.id = "t" + std::to_string(next);
        tile.input = next;
        tile.z = next;
        tile.rect = {0.75, row * 0.25, 0.25, 0.25};
        onePlusSeven.tiles.push_back(tile);
    }
    for (int col = 0; col < 3 && next <= maxInputs; ++col, ++next)
    {
        Tile tile = legacyTile();
        tile.id = "t" + std::to_string(next);
        tile.input = next;
        tile.z = next;
        tile.rect = {col * 0.25, 0.75, 0.25, 0.25};
        onePlusSeven.tiles.push_back(tile);
    }
    layouts.push_back(onePlusSeven);

    Layout twoPlusSix = legacyLayout("2+6");
    for (int i = 0; i < 2 && i < maxInputs; ++i)
    {
        Tile tile = legacyTile();
        tile.id = "t" + std::to_string(i + 1);
        tile.input = i + 1;
        tile.z = i;
        tile.rect = {i * 0.5, 0, 0.5, 0.5};
        twoPlusSix.tiles.push_back(tile);
    }
    for (int i = 0; i < 6 && (i + 2) < maxInputs; ++i)
    {
        Tile tile = legacyTile();
        tile.id = "t" + std::to_string(i + 3);
        tile.input = i + 3;
        tile.z = i + 2;
        tile.rect = {i / 6.0, 0.5, 1.0 / 6.0, 0.5};
        twoPlusSix.tiles.push_back(tile);
    }
    layouts.push_back(twoPlusSix);
    return layouts;
}

bool sameLayout(Layout const& a, Layout const& b)
{
    if (a.version != b.version || a.name != b.name || a.background != b.background || a.tiles.size() != b.tiles.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.tiles.size(); ++i)
    {
        auto const& x = a.tiles[i];
        auto const& y = b.tiles[i];
        bool const same = x.id == y.id && x.content == y.content && x.input == y.input && near(x.rect.x, y.rect.x) && near(x.rect.y, y.rect.y) &&
                          near(x.rect.w, y.rect.w) && near(x.rect.h, y.rect.h) && x.z == y.z && x.scale == y.scale && x.umd == y.umd &&
                          x.umdSource == y.umdSource && x.umdText == y.umdText && x.umdPosition == y.umdPosition && x.umdFont == y.umdFont &&
                          x.umdBg == y.umdBg && x.tallyBorder == y.tallyBorder && x.tallyLamp == y.tallyLamp && x.audioBars == y.audioBars &&
                          x.audioBarRms == y.audioBarRms && x.audioBarChannels == y.audioBarChannels && x.audioBarFirst == y.audioBarFirst &&
                          x.audioBarPosition == y.audioBarPosition && x.zoneGreen == y.zoneGreen && x.zoneAmber == y.zoneAmber &&
                          x.formatLabel == y.formatLabel && x.latency == y.latency && x.safeArea == y.safeArea && x.centre == y.centre &&
                          x.aspectMarkers == y.aspectMarkers && x.clockStyle == y.clockStyle && x.clockZone == y.clockZone &&
                          x.timecodeRate == y.timecodeRate && x.labelText == y.labelText;
        if (!same)
        {
            return false;
        }
    }
    return true;
}

std::vector<std::string> migratePresets(LayoutBook& book, int maxInputs)
{
    std::vector<std::string> migrated;
    if (book.presetRevision >= kPresetRevision)
    {
        return migrated;
    }
    auto const current = builtinPresets(maxInputs);
    for (auto& layout : book.layouts)
    {
        auto const now = std::find_if(current.begin(), current.end(), [&](Layout const& preset) { return preset.name == layout.name; });
        if (now == current.end())
        {
            continue;
        }
        // The input count may have changed since the file was written.
        bool unedited = false;
        for (int inputs = 1; inputs <= 32 && !unedited; ++inputs)
        {
            for (auto const& old : legacyPresets(inputs))
            {
                if (old.name == layout.name && sameLayout(old, layout))
                {
                    unedited = true;
                    break;
                }
            }
        }
        if (unedited)
        {
            layout = *now;
            migrated.push_back(layout.name);
        }
    }
    book.presetRevision = kPresetRevision;
    return migrated;
}
} // namespace mv
