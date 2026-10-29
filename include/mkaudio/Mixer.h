#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// mkaudio: sound event runtime (MealKit Audio, https://github.com/MealKit-Engine/MealKit-Audio, MIT).
// Puts FMOD/Wwise-style events on top of miniaudio: plays by event name + parameters, and the runtime handles clip selection (random/switch/blend containers),
// random volume/pitch, parameter curves (volume/pitch/lowpass), 3D attenuation, buses, concurrency limits/stealing, cooldowns, same-frame grouping,
// and voice management (cap on actually mixed voices, inaudible loops become virtual voices).
// This library doesn't depend on the engine (only miniaudio/stb_vorbis/the standard library. The MealKit engine checks with tools\check_libs.ps1).
// Where event definitions are read from (assets/sounds.table in MealKit), where clip files are found, and where logs go are decided by the user.
// All functions are called from one thread (the game's main thread). The audio thread is run by miniaudio, and only bus meters are shared as atomics.
namespace mkaudio {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

enum class Container : uint8_t { Random, Switch, Blend };
enum class Steal : uint8_t { Oldest, Quietest, None };

struct ClipRef {
    std::string name; // name used in messages (e.g. "sounds/hit_1.wav")
    std::string file; // file path to open (empty = missing clip: playing it gives "missing clip")
};

// One sound event. Takes positions in a left-handed system (+z forward)
struct EventDef {
    std::string name;
    std::vector<ClipRef> clips;
    Container container = Container::Random;
    std::string param; // switch/blend/curve parameter name
    float paramMin = 0.0f, paramMax = 1.0f;
    float volume = 1.0f, volumeRandom = 0.0f; // random is ± fraction
    float pitch = 1.0f, pitchRandom = 0.0f;
    float volumeAtMin = 1.0f, volumeAtMax = 1.0f; // curve
    float pitchAtMin = 1.0f, pitchAtMax = 1.0f;
    float lowpassAtMin = 0.0f, lowpassAtMax = 0.0f; // Hz, 0 = off
    bool spatial = true;
    float minDistance = 2.0f, maxDistance = 40.0f;
    std::string bus = "sfx";
    bool loop = false;
    bool stream = false; // streams from disk (music). Otherwise decoded into memory on first use
    uint32_t priority = 50;
    uint32_t maxInstances = 8;
    Steal steal = Steal::Oldest;
    float cooldown = 0.0f; // seconds
    std::string subtitle;  // subtitle key (none if empty)
    float subtitleSeconds = 2.5f;
};

// Bus definition ("mixbus <name>" in the event file). Undefined bus names are created under master with volume 1
struct BusDef {
    std::string name;
    std::string parent = "master";
    float volume = 1.0f; // mix volume (sound designer). Multiplied with the player volume (setBusVolume)
};

struct Param {
    const char* name;
    float value;
};

// One playing sound. Becomes invalid when it ends or is cut (later calls are ignored).
struct Handle {
    uint32_t index = 0xFFFFFFFFu;
    uint32_t generation = 0;
    bool valid() const { return index != 0xFFFFFFFFu; }
};

enum class LogLevel : uint8_t { Info, Warn };

struct Desc {
    uint32_t maxVoices = 32; // number of actually mixed voices
    bool muted = false;
    // Mixes without a device (tests, offline rendering): sound is pulled with render(). sampleRate is used then (the device's value if there is a device)
    bool noDevice = false;
    uint32_t sampleRate = 48000;
    uint64_t seed = 0;                                             // cosmetic randomness (from the time if 0)
    std::function<void(LogLevel level, const std::string&)> log; // writes nothing if absent
};

struct VoiceInfo {
    std::string event;
    bool virtualVoice = false; // not mixed, only the playback position is counted (far, quiet, or over the limit)
    bool loop = false;
    float audibility = 0.0f; // volume × distance attenuation
    float distance = 0.0f;
    double age = 0.0;
};
struct CullInfo {
    std::string event;
    const char* reason; // "cooldown", "max instances", "max instances (stolen)", "voice limit", "same frame", "missing event", "missing clip", "inaudible"
    double time = 0.0;
};
struct BusInfo {
    std::string name;
    float volume = 1.0f; // player volume (setBusVolume)
    float peak = 0.0f;   // recent output peak (0..1+)
    float mix = 1.0f;    // mix volume (BusDef)
    std::string parent;  // empty for master
};
// A sound with a subtitle played (same keys merge into one line and extend the time)
struct Subtitle {
    std::string key;
    float secondsLeft = 0.0f;
};
struct Stats {
    uint32_t realVoices = 0;
    uint32_t virtualVoices = 0;
    uint32_t playsThisFrame = 0;
    uint32_t culledThisFrame = 0;
    double updateMs = 0.0;
};

class Mixer {
public:
    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // Opens the default output device. Buses master (top), music, sfx, ui, voice. false if there's no device (later calls do nothing)
    bool init(const Desc& desc = {});
    void shutdown();
    bool ready() const;

    // Replaces event definitions (initially, and when the definition file changes). Cooldowns/last clips carry over by name,
    // and playing sounds find their new definitions by name (vanished events stop). Clips to decode into memory are decoded here
    void setEvents(std::vector<EventDef> events);
    // Replaces bus definitions (parent/mix volume). Missing buses are created. Cyclic parents go under master
    void setBuses(const std::vector<BusDef>& buses);

    // Play request. The actual start happens in update() (if the same event is played many times in one frame, up to the limit, loudest first)
    Handle play(std::string_view event, Vec3 position, std::initializer_list<Param> params = {});
    Handle play2D(std::string_view event, std::initializer_list<Param> params = {});
    void setParam(Handle handle, std::string_view name, float value);
    void setPosition(Handle handle, Vec3 position);
    void stop(Handle handle, float fadeSeconds = 0.0f);
    void stopAll();
    bool playing(Handle handle) const;
    void setGlobalParam(std::string_view name, float value); // parameters an event couldn't find on the sound

    void setListener(Vec3 position, Vec3 forward);
    void setBusVolume(std::string_view bus, float volume);
    float busVolume(std::string_view bus) const;
    void setMuted(bool muted);
    bool muted() const;
    const std::vector<Subtitle>& subtitles() const;

    // Once per frame: handles play requests, manages voices, cleans up finished sounds (dt in seconds)
    void update(double dt);
    // When Desc::noDevice: writes frames frames of mixed sound into out (frames × channels() floats). Returns the number of frames written
    uint64_t render(float* out, uint64_t frames);

    // For inspection (editor panels etc.)
    std::vector<std::string> eventNames() const;
    const EventDef* event(std::string_view name) const;
    void voices(std::vector<VoiceInfo>& out) const;
    const std::deque<CullInfo>& culls() const;
    void buses(std::vector<BusInfo>& out) const;
    const Stats& stats() const;
    uint32_t sampleRate() const;
    uint32_t channels() const;
    uint32_t maxVoices() const;

private:
    struct Impl;
    Impl* m = nullptr;
};

} // namespace mkaudio
