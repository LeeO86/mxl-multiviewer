#pragma once

#include <string>

namespace mv
{
struct HttpResult
{
    int status = 0;
    std::string body;
};

HttpResult httpGet(std::string const& url, int timeoutMs);
} // namespace mv
