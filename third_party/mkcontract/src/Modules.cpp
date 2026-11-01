#include "mkcontract/Modules.h"

#include <cstdlib>

namespace mkcontract {

ModuleList parseModuleList(const std::string& output) {
    ModuleList list;
    size_t start = 0;
    while (start < output.size()) {
        size_t nl = output.find('\n', start);
        if (nl == std::string::npos) nl = output.size();
        std::string line = output.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        size_t from = 0;
        for (size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', from)) {
            f.push_back(line.substr(from, tab - from));
            from = tab + 1;
        }
        f.push_back(line.substr(from));
        if (f[0] == "installed" && f.size() >= 6) {
            list.installed.push_back({f[1], f[2], f[5], std::atoi(f[3].c_str()), std::atoi(f[4].c_str())});
        } else if (f[0] == "module" && f.size() >= 10) {
            list.registry.push_back({f[1], f[2] == "-" ? std::string() : f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9]});
        } else if (f[0] == "error" && f.size() >= 2) {
            list.error = f[1];
        }
    }
    return list;
}

} // namespace mkcontract
