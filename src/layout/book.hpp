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
    // Makes `name` the book's active layout and the saved layout of heads 1..`heads`.
    bool activate(std::string const& name, int heads = 0);
    // Saves the layout chosen for one head (PUT /api/v1/outputs/{h}).
    void saveHead(int head, std::string const& name);
    // The saved layout of a head, when it is still in the book.
    [[nodiscard]] std::optional<std::string> savedHead(int head) const;
    // The layout a head starts on (SPECIFICATION.md §6.1): its saved layout, else the layout
    // configured for that head (MV_OUT<h>_LAYOUT, when `configuredForHead`), else the book's
    // active layout (MV_ACTIVE_LAYOUT until a layout file exists).
    [[nodiscard]] std::string startLayout(int head, std::string const& configured, bool configuredForHead) const;
    std::optional<std::string> upsert(Layout layout);
    std::optional<std::string> erase(std::string const& name);
    std::optional<std::string> replaceJson(std::string const& body);
    [[nodiscard]] std::string json() const;
    [[nodiscard]] std::vector<std::string> names() const;

private:
    void persistUnlocked();
    [[nodiscard]] bool hasUnlocked(std::string const& name) const;

    int maxInputs_;
    std::string path_;
    mutable std::mutex mutex_;
    LayoutBook book_;
    std::vector<std::shared_ptr<Layout const>> published_;
};
} // namespace mv
