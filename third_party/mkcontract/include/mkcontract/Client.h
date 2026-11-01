#pragma once

#include "mkcontract/Live.h"

#include <string>

// Live connection client (target mkcontract_client, own sockets: Winsock / BSD sockets). Short requests within this computer,
// so it blocks with a short timeout and closes the connection after each response (no keep-alive, no streams).
namespace mkcontract::live {

struct HttpResult {
    int status = 0; // 0 = couldn't connect or no HTTP response
    std::string body;
};

// path is what follows the token ("audio/state", "api/hello"). A body is sent as a form (a=b&c=d)
HttpResult request(const Instance& to, const char* method, const std::string& path, const std::string& body = {},
                   int timeoutMs = 300);

// For form bodies: anything other than letters/digits/-_.~ becomes %XX
std::string urlEncode(const std::string& text);

} // namespace mkcontract::live
