#include "mkcontract/User.h"

#include "mkcontract/Files.h"

#include <cstdlib>

namespace mkcontract {
namespace {

std::string environment(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

std::string base() {
#if defined(_WIN32)
    return environment("LOCALAPPDATA");
#else
    const std::string home = environment("HOME");
#if defined(__APPLE__)
    return home.empty() ? std::string() : home + "/Library/Application Support";
#else
    const std::string xdg = environment("XDG_DATA_HOME");
    if (!xdg.empty()) return xdg;
    return home.empty() ? std::string() : home + "/.local/share";
#endif
#endif
}

} // namespace

std::string userDirectory() {
    const std::string overridden = environment("MEALKIT_USER_DIR");
    if (!overridden.empty()) return overridden;
    const std::string b = base();
    return b.empty() ? std::string() : joinPath(b, "MealKit");
}

std::string userPath(const char* name) {
    const std::string user = userDirectory();
    return user.empty() ? std::string() : joinPath(user, name);
}

std::vector<std::string> recentProjects() { return readList(userPath(kRecentProjects)); }
std::vector<std::string> favoriteProjects() { return readList(userPath(kFavoriteProjects)); }
std::vector<std::string> addedEngines() { return readList(userPath(kEngines)); }

void addRecentProject(const std::string& directory) {
    const std::string dir = normalizeDirectory(directory);
    std::vector<std::string> list = recentProjects();
    eraseDirectory(list, dir);
    list.insert(list.begin(), dir);
    if (list.size() > kMaxRecent) list.resize(kMaxRecent);
    writeList(userPath(kRecentProjects), list);
}

void removeRecentProject(const std::string& directory) {
    std::vector<std::string> list = recentProjects();
    eraseDirectory(list, directory);
    writeList(userPath(kRecentProjects), list);
    setFavoriteProject(directory, false);
}

void setFavoriteProject(const std::string& directory, bool favorite) {
    std::vector<std::string> list = favoriteProjects();
    eraseDirectory(list, directory);
    if (favorite) list.push_back(normalizeDirectory(directory));
    writeList(userPath(kFavoriteProjects), list);
}

void replaceProjectDirectory(const std::string& from, const std::string& to) {
    const std::string b = normalizeDirectory(to);
    for (const char* name : {kRecentProjects, kFavoriteProjects}) {
        std::vector<std::string> list = readList(userPath(name));
        for (std::string& d : list)
            if (sameDirectory(d, from)) d = b;
        writeList(userPath(name), list);
    }
}

void addEngine(const std::string& directory) {
    std::vector<std::string> list = addedEngines();
    if (containsDirectory(list, directory)) return;
    list.push_back(normalizeDirectory(directory));
    writeList(userPath(kEngines), list);
}

std::string editorSetting(const char* key) { return readKey(userPath(kEditorSettings), key); }

std::vector<std::string> toolPaths() { return readKeys(userPath(kEditorSettings), "toolPath"); }

} // namespace mkcontract
