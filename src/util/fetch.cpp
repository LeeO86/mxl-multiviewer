#include "util/fetch.hpp"

#include "version.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>

namespace mv
{
namespace
{
struct Sink
{
    std::string* body = nullptr;
    std::size_t maxBytes = 0;
    bool tooLarge = false;
};

std::size_t onData(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& sink = *static_cast<Sink*>(user);
    std::size_t const bytes = size * count;
    if (sink.body->size() + bytes > sink.maxBytes)
    {
        sink.tooLarge = true;
        return 0; // aborts the transfer
    }
    sink.body->append(data, bytes);
    return bytes;
}
} // namespace

bool httpUrl(std::string const& url)
{
    if (url.size() > 2048 || std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
    {
        return false;
    }
    std::string scheme = url.substr(0, url.find("://"));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto const rest = url.find("://") == std::string::npos ? std::string{} : url.substr(url.find("://") + 3);
    return (scheme == "http" || scheme == "https") && !rest.empty() && rest.front() != '/';
}

FetchResult fetchUrl(std::string const& url, std::size_t maxBytes, int connectMs, int totalMs)
{
    FetchResult result;
    if (!httpUrl(url))
    {
        result.error = "only http and https URLs are fetched";
        return result;
    }
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (curl == nullptr)
    {
        result.error = "libcurl is not available";
        return result;
    }
    Sink sink{&result.body, maxBytes, false};
    auto* handle = curl.get();
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(connectMs));
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(totalMs));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(maxBytes));
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "mxl-multiviewer/" MV_VERSION);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, onData);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
    char message[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, message);
    auto const code = curl_easy_perform(handle);
    if (sink.tooLarge || code == CURLE_FILESIZE_EXCEEDED)
    {
        result.error = "the file is too large";
        result.body.clear();
        return result;
    }
    if (code != CURLE_OK)
    {
        result.error = code == CURLE_OPERATION_TIMEDOUT ? std::string("timeout") : message[0] != '\0' ? std::string(message) : curl_easy_strerror(code);
        result.body.clear();
        return result;
    }
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
    char* type = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_CONTENT_TYPE, &type) == CURLE_OK && type != nullptr)
    {
        result.contentType = type;
    }
    return result;
}
} // namespace mv
