#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Tool manifests (docs/CONTRACT.md "Tool manifest"). Each tool folder has mealkit-tool.txt:
//   # MealKit tool manifest
//   @format tool 1
//   name Audio
//   exe MealKitAudio.exe        (relative to the manifest folder, or absolute)
//   version 0.2.0
//   protocol 1                  (live connection contract version, live::kProtocol)
//   opens .events .wav          (optional: file extensions this tool opens)
// A tool is opened with: "<exe>" --project "<project folder>"
namespace mkcontract {

inline constexpr const char* kToolManifest = "mealkit-tool.txt";
inline constexpr const char* kToolFormat = "tool";
inline constexpr uint32_t kToolFormatVersion = 1;

struct ToolManifest {
    std::string folder; // folder holding the manifest
    std::string name, version;
    std::string exe; // absolute path
    uint32_t protocol = 0;
    std::vector<std::string> opens;    // ".events", ".wav"
    std::vector<std::string> warnings; // unknown keys, newer format version (readers log them)
};

// Reads <folder>/mealkit-tool.txt. false with a reason if it can't be read or name, exe or protocol is missing
bool readToolManifest(const std::string& folder, ToolManifest& out, std::string* error = nullptr);

// Folders searched for tools, in order: extra (command line), <user folder>/tools, toolPath lines of editor settings
std::vector<std::string> toolSearchRoots(const std::vector<std::string>& extra = {});

// A root that has a manifest is one tool; otherwise each folder directly under it is checked.
// For the same name the first found wins. Sorted by name. Unreadable manifests go to errors ("<folder>: <reason>")
std::vector<ToolManifest> findTools(const std::vector<std::string>& roots, std::vector<std::string>* errors = nullptr);

} // namespace mkcontract
