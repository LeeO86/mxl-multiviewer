#include "layout/book.hpp"

#include "layout/migrate.hpp"
#include "util/logging.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace mv
{
LayoutBookStore::LayoutBookStore(int maxInputs, std::string const& active, std::string path)
    : maxInputs_(maxInputs)
    , path_(std::move(path))
{
    book_ = defaultBook(maxInputs_, active);
    std::vector<std::string> migrated;
    if (!path_.empty())
    {
        std::ifstream in(path_);
        if (in)
        {
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            LayoutBook parsed;
            std::vector<std::string> repairs;
            if (auto const problem = parseBook(body, parsed, maxInputs_, &repairs))
            {
                // The presets run instead. Keep the file: the next save would overwrite it.
                auto const aside = path_ + ".bad";
                std::error_code ec;
                std::filesystem::rename(path_, aside, ec);
                logError("layouts_file_invalid", {{"path", path_}, {"error", *problem}, {"moved_to", ec ? std::string{} : aside}});
            }
            else
            {
                for (auto const& repair : repairs)
                {
                    logWarn("layout_repaired", {{"path", path_}, {"change", repair}});
                }
                migrated = migratePresets(parsed, maxInputs_);
                for (auto const& preset : book_.layouts)
                {
                    bool found = false;
                    for (auto const& layout : parsed.layouts)
                    {
                        if (layout.name == preset.name)
                        {
                            found = true;
                        }
                    }
                    if (!found)
                    {
                        parsed.layouts.push_back(preset);
                    }
                }
                if (!parsed.active.empty())
                {
                    book_.active = parsed.active;
                }
                book_.layouts = std::move(parsed.layouts);
                book_.heads = std::move(parsed.heads);
                book_.startLayouts = std::move(parsed.startLayouts);
                book_.presetRevision = parsed.presetRevision;
            }
        }
    }
    bool activeOk = false;
    for (auto const& layout : book_.layouts)
    {
        published_.push_back(std::make_shared<Layout const>(layout));
        if (layout.name == book_.active)
        {
            activeOk = true;
        }
    }
    if (!activeOk && !book_.layouts.empty())
    {
        book_.active = book_.layouts.front().name;
    }
    if (!migrated.empty())
    {
        // The file as an older release wrote it stays next to it, once.
        auto const backup = path_ + ".bak";
        std::error_code ec;
        if (!std::filesystem::exists(backup, ec))
        {
            std::filesystem::copy_file(path_, backup, ec);
        }
        std::string names;
        for (auto const& name : migrated)
        {
            names += (names.empty() ? "" : ",") + name;
        }
        logInfo("layouts_migrated", {{"path", path_}, {"layouts", names}, {"backup", backup}});
        persistUnlocked();
    }
}

std::shared_ptr<Layout const> LayoutBookStore::layout(std::string const& name) const
{
    std::lock_guard lock{mutex_};
    for (auto const& item : published_)
    {
        if (item->name == name)
        {
            return item;
        }
    }
    return published_.empty() ? nullptr : published_.front();
}

bool LayoutBookStore::has(std::string const& name) const
{
    std::lock_guard lock{mutex_};
    return hasUnlocked(name);
}

bool LayoutBookStore::hasUnlocked(std::string const& name) const
{
    for (auto const& item : published_)
    {
        if (item->name == name)
        {
            return true;
        }
    }
    return false;
}

void LayoutBookStore::saveHead(int head, std::string const& name)
{
    std::lock_guard lock{mutex_};
    book_.heads[head] = name;
    persistUnlocked();
}

std::optional<std::string> LayoutBookStore::savedHead(int head) const
{
    std::lock_guard lock{mutex_};
    auto const it = book_.heads.find(head);
    if (it != book_.heads.end() && hasUnlocked(it->second))
    {
        return it->second;
    }
    return std::nullopt;
}

bool LayoutBookStore::setStartLayout(int head, std::optional<std::string> const& name)
{
    std::lock_guard lock{mutex_};
    if (name && !hasUnlocked(*name))
    {
        return false;
    }
    if (name)
    {
        book_.startLayouts[head] = *name;
    }
    else
    {
        book_.startLayouts.erase(head);
    }
    persistUnlocked();
    return true;
}

std::optional<std::string> LayoutBookStore::startLayoutOf(int head) const
{
    std::lock_guard lock{mutex_};
    auto const it = book_.startLayouts.find(head);
    if (it != book_.startLayouts.end() && hasUnlocked(it->second))
    {
        return it->second;
    }
    return std::nullopt;
}

std::string LayoutBookStore::startLayout(int head, std::string const& configured, bool configuredForHead, std::string const& pinned) const
{
    std::lock_guard lock{mutex_};
    if (!pinned.empty() && hasUnlocked(pinned))
    {
        return pinned;
    }
    for (auto const* chosen : {&book_.startLayouts, &book_.heads})
    {
        if (auto const it = chosen->find(head); it != chosen->end() && hasUnlocked(it->second))
        {
            return it->second;
        }
    }
    if (configuredForHead && hasUnlocked(configured))
    {
        return configured;
    }
    return book_.active;
}

std::string LayoutBookStore::activeName() const
{
    std::lock_guard lock{mutex_};
    return book_.active;
}

bool LayoutBookStore::activate(std::string const& name, int heads)
{
    std::lock_guard lock{mutex_};
    for (auto const& layout : book_.layouts)
    {
        if (layout.name == name)
        {
            book_.active = name;
            for (int head = 1; head <= heads; ++head)
            {
                book_.heads[head] = name;
            }
            persistUnlocked();
            return true;
        }
    }
    return false;
}

std::optional<std::string> LayoutBookStore::upsert(Layout layout)
{
    if (auto const problem = validateLayout(layout, maxInputs_))
    {
        return problem;
    }
    std::lock_guard lock{mutex_};
    bool replaced = false;
    for (std::size_t i = 0; i < book_.layouts.size(); ++i)
    {
        if (book_.layouts[i].name == layout.name)
        {
            book_.layouts[i] = layout;
            published_[i] = std::make_shared<Layout const>(layout);
            replaced = true;
        }
    }
    if (!replaced)
    {
        book_.layouts.push_back(layout);
        published_.push_back(std::make_shared<Layout const>(layout));
    }
    persistUnlocked();
    return std::nullopt;
}

std::optional<std::string> LayoutBookStore::erase(std::string const& name)
{
    std::lock_guard lock{mutex_};
    if (name == book_.active)
    {
        return std::string("cannot delete the active layout");
    }
    for (std::size_t i = 0; i < book_.layouts.size(); ++i)
    {
        if (book_.layouts[i].name == name)
        {
            book_.layouts.erase(book_.layouts.begin() + static_cast<std::ptrdiff_t>(i));
            published_.erase(published_.begin() + static_cast<std::ptrdiff_t>(i));
            std::erase_if(book_.heads, [&](auto const& entry) { return entry.second == name; });
            std::erase_if(book_.startLayouts, [&](auto const& entry) { return entry.second == name; });
            persistUnlocked();
            return std::nullopt;
        }
    }
    return std::string("layout not found");
}

std::string LayoutBookStore::json() const
{
    std::lock_guard lock{mutex_};
    return bookToJson(book_);
}

std::vector<std::string> LayoutBookStore::names() const
{
    std::lock_guard lock{mutex_};
    std::vector<std::string> names;
    for (auto const& layout : book_.layouts)
    {
        names.push_back(layout.name);
    }
    return names;
}

std::optional<std::string> LayoutBookStore::replaceJson(std::string const& body)
{
    LayoutBook parsed;
    if (auto const error = parseBook(body, parsed, maxInputs_))
    {
        return error;
    }
    if (parsed.layouts.empty())
    {
        return std::string("the layout book has no layouts");
    }
    bool activeFound = false;
    for (auto const& layout : parsed.layouts)
    {
        activeFound = activeFound || layout.name == parsed.active;
    }
    if (!activeFound)
    {
        parsed.active = parsed.layouts.front().name;
    }
    // An old export gets today's preset defaults, as the layout file does at start.
    auto const migrated = migratePresets(parsed, maxInputs_);
    if (!migrated.empty())
    {
        std::string names;
        for (auto const& name : migrated)
        {
            names += (names.empty() ? "" : ",") + name;
        }
        logInfo("layouts_migrated", {{"source", "import"}, {"layouts", names}});
    }
    std::lock_guard lock{mutex_};
    book_ = std::move(parsed);
    published_.clear();
    for (auto const& layout : book_.layouts)
    {
        published_.push_back(std::make_shared<Layout const>(layout));
    }
    persistUnlocked();
    return std::nullopt;
}

void LayoutBookStore::persistUnlocked()
{
    if (path_.empty())
    {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    auto const tmp = path_ + ".tmp";
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
    {
        return;
    }
    out << bookToJson(book_);
    out.close();
    std::rename(tmp.c_str(), path_.c_str());
}
} // namespace mv
