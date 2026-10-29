#pragma once

#include "Json.h"

#include <cstdint>
#include <string>
#include <vector>

// Live connection client (MealKit tool contract: project build/live/<kind>-<pid>.json → http://127.0.0.1:<port>/<token>/...).
// Short requests within this computer, so it blocks on the main thread with a short timeout.
namespace mkaudio::tool {

struct Instance {
    std::string kind; // "game", "editor"
    std::string project, engine, url, token;
    uint32_t pid = 0;
    uint16_t port = 0;
    uint32_t protocol = 0;
};

struct HttpResult {
    int status = 0; // 0 = couldn't connect
    std::string body;
};

inline constexpr uint32_t kLiveProtocol = 1; // engine live::kProtocol, protocol in mealkit-tool.txt

// The project's connection info files (the process may be gone even if the file remains: a failed request means it's gone)
std::vector<Instance> findInstances(const std::string& projectDir);

// path is the path after the token ("audio/state"). body is the POST body
HttpResult request(const Instance& to, const char* method, const std::string& path, const std::string& body = {}, int timeoutMs = 300);

// For form bodies (a=b&c=d): anything other than letters/digits/-_.~ becomes %XX
std::string urlEncode(const std::string& text);

} // namespace mkaudio::tool
