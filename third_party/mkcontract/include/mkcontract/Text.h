#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// MealKit text files (docs/CONTRACT.md "Text files"): UTF-8, "#" comment lines, a format line "@format <kind> <N>" as the first line after the
// comments (no line means version 1), then "key value" lines. Writers always write the latest version; readers migrate older ones.
namespace mkcontract {

std::string trim(std::string_view s);

// Lines of a text file: \r and a UTF-8 BOM (added by Notepad/PowerShell) removed, trimmed, empty lines skipped
std::vector<std::string> splitLines(std::string_view text);

struct FormatLine {
    std::string kind;
    uint32_t version = 0;
};

enum class FormatParse {
    NotFormat, // the line doesn't start with "@"
    Ok,
    Malformed, // starts with "@" but isn't "@format <kind> <N>" with N >= 1 (callers warn and ignore the line)
};

// line: one trimmed line
FormatParse parseFormatLine(std::string_view line, FormatLine& out);
// "@format <kind> <N>" (no newline)
std::string formatLine(std::string_view kind, uint32_t version);

// "key value" line: the key ends at the first space, tab or "=", the value is the rest (trimmed, leading "=" skipped).
// false for comments (#), format lines (@) and empty lines
bool splitKeyValue(std::string_view line, std::string& key, std::string& value);

// Words separated by spaces/tabs ("opens .table .wav")
std::vector<std::string> splitWords(std::string_view text);

} // namespace mkcontract
