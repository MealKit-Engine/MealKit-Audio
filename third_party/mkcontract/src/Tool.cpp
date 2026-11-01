#include "mkcontract/Tool.h"

#include "mkcontract/Files.h"
#include "mkcontract/Text.h"
#include "mkcontract/User.h"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace mkcontract {
namespace fs = std::filesystem;

bool readToolManifest(const std::string& folder, ToolManifest& out, std::string* error) {
    out = ToolManifest{};
    out.folder = normalizeDirectory(folder);
    std::string text;
    if (!readFile(joinPath(out.folder, kToolManifest), text)) {
        if (error) *error = "cannot read " + std::string(kToolManifest);
        return false;
    }
    std::string exe;
    for (const std::string& line : splitLines(text)) {
        FormatLine format;
        switch (parseFormatLine(line, format)) {
        case FormatParse::Ok:
            if (format.kind != kToolFormat) out.warnings.push_back("this is a '" + format.kind + "' file, expected 'tool'");
            else if (format.version > kToolFormatVersion)
                out.warnings.push_back("written in tool format " + std::to_string(format.version) + ", this reader knows up to " +
                                       std::to_string(kToolFormatVersion) + " - unknown keys are ignored");
            continue;
        case FormatParse::Malformed: out.warnings.push_back("expected \"@format tool <version>\", got \"" + line + "\""); continue;
        case FormatParse::NotFormat: break;
        }
        std::string key, value;
        if (!splitKeyValue(line, key, value)) continue;
        if (key == "name") out.name = value;
        else if (key == "exe") exe = value;
        else if (key == "version") out.version = value;
        else if (key == "protocol") {
            uint32_t n = 0;
            std::from_chars(value.data(), value.data() + value.size(), n);
            out.protocol = n;
        } else if (key == "opens") out.opens = splitWords(value);
        else out.warnings.push_back("unknown key '" + key + "' ignored");
    }
    if (out.name.empty() || exe.empty() || out.protocol == 0) {
        if (error) *error = "needs name, exe and protocol";
        return false;
    }
    const fs::path p = pathOf(exe);
    out.exe = utf8Of((p.is_absolute() ? p : pathOf(out.folder) / p).lexically_normal().make_preferred());
    return true;
}

std::vector<std::string> toolSearchRoots(const std::vector<std::string>& extra) {
    std::vector<std::string> roots = extra;
    const std::string installed = userPath(kToolsFolder);
    if (!installed.empty()) roots.push_back(installed);
    for (const std::string& p : toolPaths()) roots.push_back(p);
    return roots;
}

std::vector<ToolManifest> findTools(const std::vector<std::string>& roots, std::vector<std::string>* errors) {
    std::vector<ToolManifest> out;
    auto add = [&](const std::string& folder) {
        ToolManifest t;
        std::string error;
        if (!readToolManifest(folder, t, &error)) {
            if (errors) errors->push_back(normalizeDirectory(folder) + ": " + error);
            return;
        }
        for (const ToolManifest& known : out)
            if (known.name == t.name) return;
        out.push_back(std::move(t));
    };
    for (const std::string& root : roots) {
        if (pathExists(joinPath(root, kToolManifest))) {
            add(root);
            continue;
        }
        std::vector<std::string> folders;
        std::error_code ec;
        for (fs::directory_iterator it(pathOf(root), ec), end; !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && fs::exists(it->path() / kToolManifest, ec)) folders.push_back(utf8Of(it->path()));
        std::sort(folders.begin(), folders.end()); // directory order differs between OSes
        for (const std::string& f : folders) add(f);
    }
    std::sort(out.begin(), out.end(), [](const ToolManifest& a, const ToolManifest& b) { return a.name < b.name; });
    return out;
}

} // namespace mkcontract
