#include "mkcontract/Text.h"

#include <charconv>

namespace mkcontract {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

} // namespace

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> splitLines(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        if (!line.empty()) lines.push_back(std::move(line));
        pos = end + 1;
    }
    return lines;
}

FormatParse parseFormatLine(std::string_view line, FormatLine& out) {
    if (line.empty() || line[0] != '@') return FormatParse::NotFormat;
    const std::vector<std::string> words = splitWords(line);
    if (words.size() != 3 || words[0] != "@format") return FormatParse::Malformed;
    uint32_t n = 0;
    const char* first = words[2].data();
    const char* last = first + words[2].size();
    const auto [end, ec] = std::from_chars(first, last, n);
    if (ec != std::errc() || end != last || n == 0) return FormatParse::Malformed;
    out.kind = words[1];
    out.version = n;
    return FormatParse::Ok;
}

std::string formatLine(std::string_view kind, uint32_t version) {
    return "@format " + std::string(kind) + " " + std::to_string(version);
}

bool splitKeyValue(std::string_view line, std::string& key, std::string& value) {
    const std::string s = trim(line);
    if (s.empty() || s[0] == '#' || s[0] == '@') return false;
    const size_t end = s.find_first_of(" \t=");
    key = s.substr(0, end);
    const size_t start = end == std::string::npos ? std::string::npos : s.find_first_not_of(" \t=", end);
    value = start == std::string::npos ? std::string() : s.substr(start);
    return true;
}

std::vector<std::string> splitWords(std::string_view text) {
    std::vector<std::string> words;
    size_t a = 0;
    while (a < text.size()) {
        while (a < text.size() && isSpace(text[a])) ++a;
        size_t b = a;
        while (b < text.size() && !isSpace(text[b])) ++b;
        if (b > a) words.emplace_back(text.substr(a, b - a));
        a = b;
    }
    return words;
}

} // namespace mkcontract
