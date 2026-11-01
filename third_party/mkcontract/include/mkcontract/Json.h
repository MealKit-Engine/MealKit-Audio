#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

// Small JSON for the live connection (info files, requests, responses). Not a general-purpose JSON library: no streaming, depth <= 32.
namespace mkcontract {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    // Member of an object (a null value if missing or not an object)
    const Json& operator[](const std::string& key) const;
    std::string str(const std::string& fallback = {}) const { return type == Type::String ? string : fallback; }
    double num(double fallback = 0.0) const { return type == Type::Number ? number : fallback; }
    bool flag(bool fallback = false) const { return type == Type::Bool ? boolean : fallback; }

    // The whole text must be one value (whitespace around it allowed). \uXXXX becomes UTF-8
    static bool parse(std::string_view text, Json& out);
};

// Writing: a quoted, escaped string / an array of strings
std::string jsonString(std::string_view s);
std::string jsonList(const std::vector<std::string>& items);

} // namespace mkcontract
