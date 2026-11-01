#include "mkcontract/Live.h"

#include "mkcontract/Files.h"
#include "mkcontract/Json.h"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace mkcontract::live {
namespace fs = std::filesystem;

std::string infoFileName(std::string_view kind, uint32_t pid) { return std::string(kind) + "-" + std::to_string(pid) + ".json"; }

uint32_t infoFilePid(std::string_view fileName) {
    const size_t dash = fileName.rfind('-');
    if (dash == std::string_view::npos) return 0;
    uint32_t pid = 0;
    std::from_chars(fileName.data() + dash + 1, fileName.data() + fileName.size(), pid);
    return pid;
}

std::string infoJson(const Instance& i) {
    return "{\"protocol\":" + std::to_string(i.protocol) + ",\"kind\":" + jsonString(i.kind) + ",\"pid\":" + std::to_string(i.pid) +
           ",\"port\":" + std::to_string(i.port) + ",\"token\":" + jsonString(i.token) + ",\"project\":" + jsonString(i.project) +
           ",\"engine\":" + jsonString(i.engine) + ",\"url\":" + jsonString(i.url) + "}\n";
}

bool parseInfo(std::string_view json, Instance& out) {
    out = Instance{};
    Json j;
    if (!Json::parse(json, j)) return false;
    out.kind = j["kind"].str();
    out.project = j["project"].str();
    out.engine = j["engine"].str();
    out.url = j["url"].str();
    out.token = j["token"].str();
    out.protocol = static_cast<uint32_t>(j["protocol"].num());
    out.pid = static_cast<uint32_t>(j["pid"].num());
    out.port = static_cast<uint16_t>(j["port"].num());
    return out.port != 0 && !out.token.empty();
}

std::vector<Instance> findInstances(const std::string& projectDirectory) {
    std::vector<std::string> files;
    std::error_code ec;
    for (fs::directory_iterator it(pathOf(projectDirectory) / kInfoFolder, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".json") files.push_back(utf8Of(it->path()));
    std::sort(files.begin(), files.end());
    std::vector<Instance> out;
    for (const std::string& f : files) {
        std::string text;
        Instance i;
        if (readFile(f, text) && parseInfo(text, i)) out.push_back(std::move(i));
    }
    return out;
}

} // namespace mkcontract::live
