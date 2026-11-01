#include "mkcontract/Json.h"

#include <cstdio>
#include <cstdlib>

namespace mkcontract {
namespace {

constexpr int kMaxDepth = 32;

void skip(std::string_view t, size_t& p) {
    while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\r' || t[p] == '\n')) ++p;
}

bool hex4(std::string_view t, size_t& p, unsigned& out) {
    if (p + 4 > t.size()) return false;
    out = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char c = t[p + i];
        out <<= 4;
        if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
        else return false;
    }
    p += 4;
    return true;
}

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool text(std::string_view t, size_t& p, std::string& out) {
    if (p >= t.size() || t[p] != '"') return false;
    ++p;
    while (p < t.size() && t[p] != '"') {
        const char c = t[p++];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (p >= t.size()) return false;
        const char e = t[p++];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
            unsigned cp = 0;
            if (!hex4(t, p, cp)) return false;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') { // surrogate pair
                size_t q = p + 2;
                unsigned low = 0;
                if (hex4(t, q, low) && low >= 0xDC00 && low < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    p = q;
                }
            }
            appendUtf8(out, cp);
            break;
        }
        default: out += e; // \" \\ \/
        }
    }
    if (p >= t.size()) return false;
    ++p;
    return true;
}

bool value(std::string_view t, size_t& p, Json& out, int depth) {
    if (depth > kMaxDepth) return false;
    skip(t, p);
    if (p >= t.size()) return false;
    const char c = t[p];
    if (c == '{') {
        out.type = Json::Type::Object;
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
            if (p < t.size() && t[p] == ',') {
                ++p;
                continue;
            }
            if (p < t.size() && t[p] == '}') return ++p, true;
            return false;
        }
    }
    if (c == '[') {
        out.type = Json::Type::Array;
        ++p;
        skip(t, p);
        if (p < t.size() && t[p] == ']') return ++p, true;
        for (;;) {
            out.array.emplace_back();
            if (!value(t, p, out.array.back(), depth + 1)) return false;
            skip(t, p);
            if (p < t.size() && t[p] == ',') {
                ++p;
                continue;
            }
            if (p < t.size() && t[p] == ']') return ++p, true;
            return false;
        }
    }
    if (c == '"') {
        out.type = Json::Type::String;
        return text(t, p, out.string);
    }
    if (t.substr(p, 4) == "true") return out.type = Json::Type::Bool, out.boolean = true, p += 4, true;
    if (t.substr(p, 5) == "false") return out.type = Json::Type::Bool, out.boolean = false, p += 5, true;
    if (t.substr(p, 4) == "null") return out.type = Json::Type::Null, p += 4, true;
    const std::string rest(t.substr(p, 64));
    char* end = nullptr;
    out.number = std::strtod(rest.c_str(), &end);
    if (end == rest.c_str()) return false;
    out.type = Json::Type::Number;
    p += static_cast<size_t>(end - rest.c_str());
    return true;
}

} // namespace

const Json& Json::operator[](const std::string& key) const {
    static const Json none;
    const auto it = object.find(key);
    return it == object.end() ? none : it->second;
}

bool Json::parse(std::string_view t, Json& out) {
    out = Json{};
    size_t p = 0;
    if (!value(t, p, out, 0)) return false;
    skip(t, p);
    return p == t.size();
}

std::string jsonString(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else out += c;
        }
    }
    return out + "\"";
}

std::string jsonList(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + jsonString(items[i]);
    return out + "]";
}

} // namespace mkcontract
