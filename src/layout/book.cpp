#include "layout/book.hpp"

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
    if (!path_.empty())
    {
        std::ifstream in(path_);
        if (in)
        {
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            LayoutBook parsed;
            if (!parseBook(body, parsed, maxInputs_))
            {
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

std::string LayoutBookStore::activeName() const
{
    std::lock_guard lock{mutex_};
    return book_.active;
}

bool LayoutBookStore::activate(std::string const& name)
{
    std::lock_guard lock{mutex_};
    for (auto const& layout : book_.layouts)
    {
        if (layout.name == name)
        {
            book_.active = name;
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
