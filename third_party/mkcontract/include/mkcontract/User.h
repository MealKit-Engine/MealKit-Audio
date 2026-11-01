#pragma once

#include <cstddef>
#include <string>
#include <vector>

// The per-user MealKit folder shared by the editor, MealKit Manager and tools (docs/CONTRACT.md "User folder").
namespace mkcontract {

inline constexpr const char* kRecentProjects = "recent_projects.txt";     // project folders, most recently opened first
inline constexpr const char* kFavoriteProjects = "favorite_projects.txt"; // project folders
inline constexpr const char* kEngines = "engines.txt";                    // engine folders added by hand (Manager)
inline constexpr const char* kEditorSettings = "editor_settings.txt";     // "key value" lines (language, toolPath, ...)
inline constexpr const char* kToolsFolder = "tools";                      // installed tools, one folder each
inline constexpr size_t kMaxRecent = 100;

// MEALKIT_USER_DIR if set (tests), else <base>/MealKit with base = Windows %LOCALAPPDATA%, macOS ~/Library/Application Support,
// other $XDG_DATA_HOME or ~/.local/share. Empty if none of them is known
std::string userDirectory();
// <user folder>/<name>
std::string userPath(const char* name);

std::vector<std::string> recentProjects();
std::vector<std::string> favoriteProjects();
std::vector<std::string> addedEngines();

void addRecentProject(const std::string& directory);    // to the top, at most kMaxRecent
void removeRecentProject(const std::string& directory); // also removed from favorites (the folder itself stays)
void setFavoriteProject(const std::string& directory, bool favorite);
void replaceProjectDirectory(const std::string& from, const std::string& to); // after a project folder was renamed/moved
void addEngine(const std::string& directory);

// Editor settings values
std::string editorSetting(const char* key);
std::vector<std::string> toolPaths(); // "toolPath <folder>" lines

} // namespace mkcontract
