#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "layout/model.hpp"

namespace mv
{
class LayoutBookStore
{
public:
    LayoutBookStore(int maxInputs, std::string const& active, std::string path);

    // The named layout, or the first one of the book when the name is unknown.
    [[nodiscard]] std::shared_ptr<Layout const> layout(std::string const& name) const;
    [[nodiscard]] bool has(std::string const& name) const;
    [[nodiscard]] std::string activeName() const;
    bool activate(std::string const& name);
    std::optional<std::string> upsert(Layout layout);
    std::optional<std::string> erase(std::string const& name);
    std::optional<std::string> replaceJson(std::string const& body);
    [[nodiscard]] std::string json() const;
    [[nodiscard]] std::vector<std::string> names() const;

private:
    void persistUnlocked();

    int maxInputs_;
    std::string path_;
    mutable std::mutex mutex_;
    LayoutBook book_;
    std::vector<std::shared_ptr<Layout const>> published_;
};
} // namespace mv
