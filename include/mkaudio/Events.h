#pragma once

#include "mkaudio/Mixer.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Event file (assets/sounds.events in MealKit projects). Edited by the authoring tool MealKit Audio and read by the game as is (no export).
// It's text that merges in git:
//   # MealKit audio events. Edit them in MealKit Audio or by hand.
//   @format audio-events 1
//
//   mixbus music
//   volume 0.8
//
//   event hit
//   clips sounds/hit_1.wav sounds/hit_2.wav
//   container random
//   volumeRandom 0.15
//   bus sfx
//
//   snapshot pause
//   fade 0.3
//   volume music 0.5
//   lowpass sfx 800
//
//   music explore
//   stems music/explore_base.ogg music/explore_drums.ogg
//   bpm 96
//   stemParam tension
// "event <name>"/"mixbus <name>"/"snapshot <name>"/"music <name>" open a block ("bus" is the key setting an event's output bus, so bus definitions are mixbus), followed by "key value" lines. Keys not written are defaults (initial values of EventDef/BusDef).
// Event keys are EventDef field names as is, clips are space-separated paths (relative to the user; assets-relative in MealKit), container is random/switch/blend,
// steal is oldest/quietest/none, booleans are true/false. Snapshot lines are "volume <bus> <v>" and "lowpass <bus> <Hz>"; music keys are MusicDef
// field names (stems are clips, transition is immediate/beat/bar). Writing outputs only keys differing from defaults, in a fixed key order.
// When the format changes, bump kEventsFormatVersion and add a migrate for old versions on the reading side (format line of the MealKit tool contract).
// Version 2 added occlusion, bus reverb/ducking, snapshots and music; version 1 files read as they are.
namespace mkaudio {

inline constexpr const char* kEventsFormatKind = "audio-events";
inline constexpr uint32_t kEventsFormatVersion = 2;

struct EventFile {
    std::vector<BusDef> buses;
    std::vector<EventDef> events; // file order
    std::vector<SnapshotDef> snapshots;
    std::vector<MusicDef> music;
};

struct ParseMessage {
    uint32_t line = 0;
    bool error = false; // false = warning (reads what it knows and continues)
    std::string text;
};

// Reads text. Bad lines are left in messages and skipped (false if the format line is a different kind entirely). Clip file is empty (resolveClips)
bool parseEvents(std::string_view text, EventFile& out, std::vector<ParseMessage>* messages = nullptr);
std::string writeEvents(const EventFile& file);

bool loadEventFile(const std::string& path, EventFile& out, std::vector<ParseMessage>* messages = nullptr);
bool saveEventFile(const std::string& path, const EventFile& file); // writes to a temp file in the same folder and swaps it in

// Clip name (path written in the file) → file path to open. Returns an empty string if not found
void resolveClips(EventFile& file, const std::function<std::string(const std::string& name)>& resolve);

// Name validation: only letters/digits/_ - . (the file's "event <name>" line and game code's play("name"))
bool validName(std::string_view name);

const char* containerName(Container c);
const char* stealName(Steal s);
const char* quantizeName(Quantize q);

} // namespace mkaudio
