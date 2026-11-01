#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Live connection (docs/CONTRACT.md "Live connection"). Dev builds of the editor and games open an HTTP server on 127.0.0.1 and write
// <project>/build/live/<kind>-<pid>.json; tools find it there and call http://127.0.0.1:<port>/<token>/<service>/...
// The server is the engine's (mealkit/live/Server.h). This header is the shared part; the client is mkcontract/Client.h (mkcontract_client).
namespace mkcontract::live {

inline constexpr uint32_t kProtocol = 1; // contract version: bump when paths or response shapes change (manifests say which one a tool speaks)
inline constexpr const char* kInfoFolder = "build/live";

struct Instance {
    std::string kind; // "editor", "game"
    std::string project, engine, url, token;
    uint32_t protocol = 0;
    uint32_t pid = 0;
    uint16_t port = 0;
};

// "<kind>-<pid>.json" / the pid in such a name (0 if none)
std::string infoFileName(std::string_view kind, uint32_t pid);
uint32_t infoFilePid(std::string_view fileName);

// One JSON object + "\n" (what the server writes) / reading it back (false without a port and token)
std::string infoJson(const Instance& instance);
bool parseInfo(std::string_view json, Instance& out);

// Info files of a project. The process may be gone even if its file remains: a failed request means it's gone
std::vector<Instance> findInstances(const std::string& projectDirectory);

} // namespace mkcontract::live
