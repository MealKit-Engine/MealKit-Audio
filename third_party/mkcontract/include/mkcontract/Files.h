#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Files and folders as the contract names them: paths are UTF-8 std::string on every OS.
namespace mkcontract {

std::filesystem::path pathOf(const std::string& utf8);
std::string utf8Of(const std::filesystem::path& path);
std::string joinPath(const std::string& directory, const std::string& name);
bool pathExists(const std::string& path);

bool readFile(const std::string& path, std::string& out);
// Creates the parent folders. Written in binary (\n line ends on every OS)
bool writeFile(const std::string& path, std::string_view text);

// List files: one item per line (splitLines rules). Missing file = empty list
std::vector<std::string> readList(const std::string& path);
bool writeList(const std::string& path, const std::vector<std::string>& list);

// The first value of key in a "key value" file (comments and the format line skipped). Empty if missing
std::string readKey(const std::string& path, std::string_view key);
// Every value of a key that may repeat ("toolPath <folder>" lines)
std::vector<std::string> readKeys(const std::string& path, std::string_view key);

// Folder notation used in every list: absolute, native separators, no trailing separator
std::string normalizeDirectory(const std::string& directory);
// The same folder in any notation (case-insensitive on Windows)
bool sameDirectory(const std::string& a, const std::string& b);
bool containsDirectory(const std::vector<std::string>& list, const std::string& directory);
void eraseDirectory(std::vector<std::string>& list, const std::string& directory);

} // namespace mkcontract
