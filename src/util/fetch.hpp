#pragma once

#include <cstddef>
#include <string>

namespace mv
{
// True for an http:// or https:// URL of at most 2048 characters without spaces or
// control characters. Other schemes (file:, ftp:, …) are refused.
bool httpUrl(std::string const& url);

struct FetchResult
{
    // Empty when the transfer completed (any HTTP status).
    std::string error;
    long status = 0;
    std::string contentType;
    std::string body;
};

// GET over http or https with libcurl, also through redirects (at most 3, http and https
// only). A body larger than `maxBytes` is cut off and reported as an error. The proxy comes
// from the environment (https_proxy, http_proxy, no_proxy). Blocks up to `totalMs`.
FetchResult fetchUrl(std::string const& url, std::size_t maxBytes, int connectMs, int totalMs);
} // namespace mv
