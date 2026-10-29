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
// "event <name>"/"mixbus <name>" open a block ("bus" is the key setting an event's output bus, so bus definitions are mixbus), followed by "key value" lines. Keys not written are defaults (initial values of EventDef/BusDef).
// Event keys are EventDef field names as is, clips are space-separated paths (relative to the user; assets-relative in MealKit), container is random/switch/blend,
// steal is oldest/quietest/none, booleans are true/false. Writing outputs only keys differing from defaults, in a fixed key order.
// When the format changes, bump kEventsFormatVersion and add a migrate for old versions on the reading side (format line of the MealKit tool contract).
namespace mkaudio {

inline constexpr const char* kEventsFormatKind = "audio-events";
inline constexpr uint32_t kEventsFormatVersion = 1;

struct EventFile {
    std::vector<BusDef> buses;
    std::vector<EventDef> events; // file order
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

} // namespace mkaudio
