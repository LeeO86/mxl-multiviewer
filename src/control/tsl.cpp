#include "control/tsl.hpp"

#include <cstdlib>
#include <sstream>

namespace mv
{
namespace
{
std::uint16_t read16(std::uint8_t const* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void appendUtf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
} // namespace

std::vector<std::uint8_t> unwrapDle(std::uint8_t const* data, std::size_t size, bool& ok)
{
    ok = true;
    if (data == nullptr || size < 2 || data[0] != 0xfe || data[1] != 0x02)
    {
        return std::vector<std::uint8_t>(data, data + size);
    }
    std::vector<std::uint8_t> out;
    for (std::size_t i = 2; i < size; ++i)
    {
        if (data[i] == 0xfe)
        {
            if (i + 1 >= size)
            {
                ok = false;
                return {};
            }
            if (data[i + 1] == 0xfe)
            {
                out.push_back(0xfe);
                ++i;
                continue;
            }
            if (data[i + 1] == 0x03)
            {
                return out;
            }
            ok = false;
            return {};
        }
        out.push_back(data[i]);
    }
    ok = false;
    return {};
}

std::vector<std::vector<std::uint8_t>> pullTslFrames(std::vector<std::uint8_t>& buffer)
{
    std::vector<std::vector<std::uint8_t>> frames;
    while (true)
    {
        std::size_t start = 0;
        bool found = false;
        for (; start + 1 < buffer.size(); ++start)
        {
            if (buffer[start] == 0xfe && buffer[start + 1] == 0x02)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            if (!buffer.empty() && buffer.back() == 0xfe)
            {
                buffer.erase(buffer.begin(), buffer.end() - 1);
            }
            else
            {
                buffer.clear();
            }
            break;
        }
        if (start > 0)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(start));
        }
        bool complete = false;
        bool resync = false;
        std::size_t end = 0;
        for (std::size_t i = 2; i < buffer.size(); ++i)
        {
            if (buffer[i] != 0xfe)
            {
                continue;
            }
            if (i + 1 >= buffer.size())
            {
                complete = false;
                break;
            }
            if (buffer[i + 1] == 0xfe)
            {
                ++i;
                continue;
            }
            if (buffer[i + 1] == 0x03)
            {
                complete = true;
                end = i + 2;
                break;
            }
            buffer.erase(buffer.begin(), buffer.begin() + 2);
            resync = true;
            break;
        }
        if (resync)
        {
            continue;
        }
        if (!complete)
        {
            break;
        }
        bool ok = false;
        auto body = unwrapDle(buffer.data(), end, ok);
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(end));
        if (ok)
        {
            frames.push_back(std::move(body));
        }
    }
    return frames;
}

TslMessage parseTsl5(std::uint8_t const* body, std::size_t size)
{
    TslMessage message;
    if (body == nullptr || size < 6)
    {
        message.error = "short";
        return message;
    }
    std::uint16_t const pbc = read16(body);
    if (static_cast<std::size_t>(pbc) + 2 > size)
    {
        message.error = "pbc";
        return message;
    }
    std::size_t const end = static_cast<std::size_t>(pbc) + 2;
    bool const unicode = (body[3] & 0x01) != 0;
    if ((body[3] & 0x02) != 0)
    {
        return message;
    }
    int const screen = read16(body + 4);
    std::size_t cursor = 6;
    while (cursor + 4 <= end)
    {
        int const index = read16(body + cursor);
        std::uint16_t const control = read16(body + cursor + 2);
        cursor += 4;
        if ((control & 0x8000) != 0)
        {
            if (cursor + 2 > end)
            {
                message.error = "control-length";
                return message;
            }
            int const length = read16(body + cursor);
            cursor += 2 + static_cast<std::size_t>(length);
            continue;
        }
        if (cursor + 2 > end)
        {
            message.error = "length";
            return message;
        }
        int const length = read16(body + cursor);
        cursor += 2;
        if (cursor + static_cast<std::size_t>(length) > end)
        {
            message.error = "text";
            return message;
        }
        TallyUpdate update;
        update.screen = screen;
        update.index = index;
        update.rh = control & 0x3;
        update.text = (control >> 2) & 0x3;
        update.lh = (control >> 4) & 0x3;
        update.brightness = (control >> 6) & 0x3;
        if (unicode)
        {
            // UTF-16LE to UTF-8 (the overlay and the API carry UTF-8). A lone surrogate is U+FFFD.
            for (int i = 0; i + 1 < length; i += 2)
            {
                std::uint32_t cp = read16(body + cursor + static_cast<std::size_t>(i));
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < length)
                {
                    std::uint32_t const low = read16(body + cursor + static_cast<std::size_t>(i) + 2);
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF)
                {
                    cp = 0xFFFD;
                }
                if (cp != 0)
                {
                    appendUtf8(update.textValue, cp);
                }
            }
        }
        else
        {
            update.textValue.assign(reinterpret_cast<char const*>(body + cursor), reinterpret_cast<char const*>(body + cursor + length));
            while (!update.textValue.empty() && update.textValue.back() == '\0')
            {
                update.textValue.pop_back();
            }
        }
        cursor += static_cast<std::size_t>(length);
        message.displays.push_back(std::move(update));
    }
    return message;
}

TslMessage parseTsl31(std::uint8_t const* data, std::size_t size)
{
    TslMessage message;
    if (data == nullptr || size < 18)
    {
        message.error = "short";
        return message;
    }
    TallyUpdate update;
    update.index = data[0] & 0x7f;
    bool const red = (data[1] & 0x01) != 0;
    bool const green = (data[1] & 0x02) != 0;
    bool const amber = (data[1] & 0x04) != 0;
    int tally = 0;
    if (amber || (red && green))
    {
        tally = 3;
    }
    else if (red)
    {
        tally = 1;
    }
    else if (green)
    {
        tally = 2;
    }
    // TSL 3.1 has one tally: both lamps and the text show it.
    update.lh = tally;
    update.rh = tally;
    update.text = tally;
    update.textValue.assign(reinterpret_cast<char const*>(data + 2), reinterpret_cast<char const*>(data + 18));
    while (!update.textValue.empty() && (update.textValue.back() == '\0' || update.textValue.back() == ' '))
    {
        update.textValue.pop_back();
    }
    message.displays.push_back(std::move(update));
    return message;
}

int effectiveTally(TallyUpdate const& update)
{
    if (update.text != 0)
    {
        return update.text;
    }
    if (update.rh != 0)
    {
        return update.rh;
    }
    return update.lh;
}

int inputForDisplay(std::string const& map, int display)
{
    if (map.empty())
    {
        return display + 1;
    }
    std::stringstream stream(map);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        auto const colon = item.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        if (std::atoi(item.c_str()) == display)
        {
            return std::atoi(item.c_str() + colon + 1);
        }
    }
    return 0;
}
} // namespace mv
