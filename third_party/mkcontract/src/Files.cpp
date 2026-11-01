#include "mkcontract/Files.h"

#include "mkcontract/Text.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace mkcontract {
namespace fs = std::filesystem;

fs::path pathOf(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

std::string utf8Of(const fs::path& path) {
    const std::u8string s = path.u8string();
    return std::string(s.begin(), s.end());
}

std::string joinPath(const std::string& directory, const std::string& name) { return utf8Of(pathOf(directory) / pathOf(name)); }

bool pathExists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(pathOf(path), ec);
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const std::string& path, std::string_view text) {
    std::error_code ec;
    const fs::path p = pathOf(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::vector<std::string> readList(const std::string& path) {
    std::string text;
    if (!readFile(path, text)) return {};
    return splitLines(text);
}

bool writeList(const std::string& path, const std::vector<std::string>& list) {
    std::string text;
    for (const std::string& line : list) text += line + "\n";
    return writeFile(path, text);
}

std::string readKey(const std::string& path, std::string_view key) {
    std::string k, v;
    for (const std::string& line : readList(path))
        if (splitKeyValue(line, k, v) && k == key) return v;
    return {};
}

std::vector<std::string> readKeys(const std::string& path, std::string_view key) {
    std::vector<std::string> out;
    std::string k, v;
    for (const std::string& line : readList(path))
        if (splitKeyValue(line, k, v) && k == key && !v.empty()) out.push_back(v);
    return out;
}

std::string normalizeDirectory(const std::string& directory) {
    if (directory.empty()) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(pathOf(directory), ec);
    if (ec) p = pathOf(directory);
    std::string s = utf8Of(p.make_preferred());
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

bool sameDirectory(const std::string& a, const std::string& b) {
    const std::string x = normalizeDirectory(a), y = normalizeDirectory(b);
#if defined(_WIN32)
    return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [](char p, char q) {
               return std::tolower(static_cast<unsigned char>(p)) == std::tolower(static_cast<unsigned char>(q));
           });
#else
    return x == y;
#endif
}

bool containsDirectory(const std::vector<std::string>& list, const std::string& directory) {
    return std::any_of(list.begin(), list.end(), [&](const std::string& d) { return sameDirectory(d, directory); });
}

void eraseDirectory(std::vector<std::string>& list, const std::string& directory) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& d) { return sameDirectory(d, directory); }),
               list.end());
}

} // namespace mkcontract
