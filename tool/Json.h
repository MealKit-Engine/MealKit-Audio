#pragma once

#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Small JSON reader (live connection info files and responses). Objects/arrays/strings/numbers/booleans/null
namespace mkaudio::tool {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json& operator[](const std::string& key) const {
        static const Json none;
        const auto it = object.find(key);
        return it == object.end() ? none : it->second;
    }
    std::string str(const std::string& fallback = {}) const { return type == Type::String ? string : fallback; }
    double num(double fallback = 0.0) const { return type == Type::Number ? number : fallback; }
    bool flag(bool fallback = false) const { return type == Type::Bool ? boolean : fallback; }

    static bool parse(std::string_view text, Json& out) {
        size_t pos = 0;
        return value(text, pos, out, 0);
    }

private:
    static void skip(std::string_view t, size_t& p) {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\r' || t[p] == '\n')) ++p;
    }
    static bool text(std::string_view t, size_t& p, std::string& out) {
        if (p >= t.size() || t[p] != '"') return false;
        ++p;
        while (p < t.size() && t[p] != '"') {
            char c = t[p++];
            if (c == '\\' && p < t.size()) {
                const char e = t[p++];
                switch (e) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'u': // ASCII only (the engine uses it only for control characters)
                    c = p + 4 <= t.size() ? static_cast<char>(std::strtol(std::string(t.substr(p, 4)).c_str(), nullptr, 16)) : '?';
                    p += 4;
                    break;
                default: c = e;
                }
            }
            out += c;
        }
        if (p >= t.size()) return false;
        ++p;
        return true;
    }
    static bool value(std::string_view t, size_t& p, Json& out, int depth) {
        if (depth > 32) return false;
        skip(t, p);
        if (p >= t.size()) return false;
        const char c = t[p];
        if (c == '{') {
            out.type = Type::Object;
            ++p;
            skip(t, p);
            if (p < t.size() && t[p] == '}') return ++p, true;
            for (;;) {
                skip(t, p);
                std::string key;
                if (!text(t, p, key)) return false;
                skip(t, p);
                if (p >= t.size() || t[p] != ':') return false;
                ++p;
                if (!value(t, p, out.object[key], depth + 1)) return false;
                skip(t, p);
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') return ++p, true;
                return false;
            }
        }
        if (c == '[') {
            out.type = Type::Array;
            ++p;
            skip(t, p);
            if (p < t.size() && t[p] == ']') return ++p, true;
            for (;;) {
                out.array.emplace_back();
                if (!value(t, p, out.array.back(), depth + 1)) return false;
                skip(t, p);
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') return ++p, true;
                return false;
            }
        }
        if (c == '"') {
            out.type = Type::String;
            return text(t, p, out.string);
        }
        if (t.substr(p, 4) == "true") return out.type = Type::Bool, out.boolean = true, p += 4, true;
        if (t.substr(p, 5) == "false") return out.type = Type::Bool, out.boolean = false, p += 5, true;
        if (t.substr(p, 4) == "null") return out.type = Type::Null, p += 4, true;
        const std::string rest(t.substr(p, 64));
        char* end = nullptr;
        out.number = std::strtod(rest.c_str(), &end);
        if (end == rest.c_str()) return false;
        out.type = Type::Number;
        p += static_cast<size_t>(end - rest.c_str());
        return true;
    }
};

} // namespace mkaudio::tool
