#pragma once

#include <string>
#include <vector>

// Source module list (docs/CONTRACT.md "Module list"): what the engine's `tools\module.ps1 search -Tsv -Project <folder>` prints,
// one tab-separated line per item. The editor [Modules] window and MealKit Manager read it. Unknown lines are skipped.
//   installed <name> <version> <modified> <pending> <source>
//   module    <name> <version or -> <latest> <latestEngine> <title> <summary> <tags> <author> <repo>
//   error     <why the registry list couldn't be fetched>
namespace mkcontract {

struct InstalledModule {
    std::string name, version, source;
    int modified = 0; // number of files the game modified after install
    int pending = 0;  // number of .new files left by an update
};

struct RegistryModule {
    std::string name;
    std::string version;      // newest version for the project's engine MINOR (blank if none)
    std::string latest;       // newest version
    std::string latestEngine; // engine MAJOR.MINOR of that version
    std::string title, summary, tags, author, repo;
};

struct ModuleList {
    std::vector<InstalledModule> installed;
    std::vector<RegistryModule> registry;
    std::string error;
};

ModuleList parseModuleList(const std::string& output);

} // namespace mkcontract
