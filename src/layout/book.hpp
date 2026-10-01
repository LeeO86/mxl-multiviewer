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

    [[nodiscard]] std::shared_ptr<Layout const> layout(std::string const& name) const;
    [[nodiscard]] std::string activeName() const;
    bool activate(std::string const& name);
    std::optional<std::string> upsert(Layout layout);
    std::optional<std::string> erase(std::string const& name);
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
