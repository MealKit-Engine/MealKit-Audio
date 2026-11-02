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
// Spatial: occlusion amounts per voice (the game casts the rays: emitters() → setOcclusion()), reverb on buses (Freeverb) with gradual changes (reverb zones).
// Mix: ducking (bus A lowers bus B while it sounds), snapshots (sets of bus volume/lowpass turned on and off with a fade).
// Interactive music: tracks with BPM/time signature, transitions at the next beat/bar scheduled to the PCM frame, stingers, stems faded by a parameter.
// This library doesn't depend on the engine (only miniaudio/stb_vorbis/the standard library. The MealKit engine checks with tools\check_libs.ps1).
// Where event definitions are read from (assets/sounds.table in MealKit), where clip files are found, and where logs go are decided by the user.
// All functions are called from one thread (the game's main thread). The audio thread is run by miniaudio, and only bus meters are shared as atomics.
namespace mkaudio {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

enum class Container : uint8_t { Random, Switch, Blend };
enum class Steal : uint8_t { Oldest, Quietest, None };
// Where a music transition or stinger lands: right away, the next beat, the next bar of the playing track
enum class Quantize : uint8_t { Immediate, Beat, Bar };

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
    // Fully occluded (setOcclusion 1): volume times this and a lowpass at this cutoff (Hz, 0 = no lowpass). Spatial events only
    float occlusionVolume = 0.35f;
    float occlusionLowpass = 1200.0f;
};

// Bus definition ("mixbus <name>" in the event file). Undefined bus names are created under master with volume 1
struct BusDef {
    std::string name;
    std::string parent = "master";
    float volume = 1.0f; // mix volume (sound designer). Multiplied with the player volume (setBusVolume)
    bool reverb = false; // reverb insert on this bus (amount from setReverb / reverb zones; dry sound passes as is)
    std::string duckBy;  // while a sound plays on this bus (or its children), this bus goes down to duckVolume
    float duckVolume = 0.4f;
    float duckAttack = 0.08f, duckRelease = 0.6f; // seconds to go down / come back
};

// Snapshot: bus values applied while it's on ("snapshot <name>" in the event file). Several on at once multiply volumes and take the lowest lowpass
struct SnapshotBus {
    std::string bus;
    float volume = 1.0f;
    float lowpass = 0.0f; // Hz, 0 = off
};
struct SnapshotDef {
    std::string name;
    float fade = 0.3f; // seconds to turn on/off
    std::vector<SnapshotBus> buses;
};

// Music track ("music <name>" in the event file). Stems are clips of the same length played in sync (looping);
// stem 0 always sounds, later stems fade in one after another as stemParam (a global parameter, e.g. tension) goes from paramMin to paramMax
struct MusicDef {
    std::string name;
    std::vector<ClipRef> stems;
    float bpm = 120.0f;
    uint32_t beatsPerBar = 4;
    float offset = 0.0f; // seconds before the first beat (pickup)
    Quantize transition = Quantize::Bar; // where playMusic switches to this track when another one plays
    float crossfade = 0.05f;             // seconds the old track fades out / this one fades in at the transition
    std::string stemParam;
    float paramMin = 0.0f, paramMax = 1.0f;
    float stemFade = 1.0f; // seconds a stem takes to follow a parameter change
    float volume = 1.0f;
    std::string bus = "music";
    bool loop = true;
};

// Reverb values (a reverb zone's settings). wet 0 = off
struct ReverbParams {
    float roomSize = 0.5f; // 0..1
    float damping = 0.5f;  // 0..1, high frequencies die faster
    float wet = 0.0f;      // 0..1
    float width = 1.0f;    // 0..1 stereo
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
    float occlusion = 0.0f; // current (smoothed) occlusion
};
// A mixed 3D voice: the game checks the line from the listener to position and passes the result to setOcclusion
struct Emitter {
    Handle handle;
    Vec3 position;
    float audibility = 0.0f;
    float occlusion = 0.0f; // current target
};
struct MusicInfo {
    std::string track; // playing (or about to start) track, empty if none
    std::string next;  // track waiting for its transition point
    float bpm = 0.0f;
    uint32_t beatsPerBar = 4;
    uint32_t bar = 0;     // from 0
    float beat = 0.0f;    // position inside the bar (0..beatsPerBar)
};
struct SnapshotInfo {
    std::string name;
    bool on = false;
    float weight = 0.0f; // 0..1 during the fade
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
    float duck = 1.0f;     // current ducking gain
    float snapshot = 1.0f; // current snapshot volume
    float lowpass = 0.0f;  // current snapshot lowpass (Hz, 0 = off)
    bool reverb = false;
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
    // Replaces snapshot / music definitions (snapshots that are on stay on by name, the playing track keeps playing if its name remains)
    void setSnapshots(std::vector<SnapshotDef> snapshots);
    void setMusic(std::vector<MusicDef> tracks);

    // Play request. The actual start happens in update() (if the same event is played many times in one frame, up to the limit, loudest first)
    Handle play(std::string_view event, Vec3 position, std::initializer_list<Param> params = {});
    Handle play2D(std::string_view event, std::initializer_list<Param> params = {});
    void setParam(Handle handle, std::string_view name, float value);
    void setPosition(Handle handle, Vec3 position);
    void stop(Handle handle, float fadeSeconds = 0.0f);
    void stopAll();
    bool playing(Handle handle) const;
    void setGlobalParam(std::string_view name, float value); // parameters an event couldn't find on the sound (also music stem parameters)

    // Occlusion: 3D voices being mixed (the game checks a few of them per frame) and the result for one (0 = clear .. 1 = blocked, eased in)
    void emitters(std::vector<Emitter>& out) const;
    void setOcclusion(Handle handle, float amount);
    // Reverb on buses with reverb set: moves to these values over seconds (entering/leaving a reverb zone)
    void setReverb(const ReverbParams& reverb, float seconds = 0.5f);
    const ReverbParams& reverb() const; // target
    void setSnapshot(std::string_view name, bool on);
    bool snapshot(std::string_view name) const;

    // Music: switches to the track at its transition point (its MusicDef::transition, or the given one). The same track again does nothing
    void playMusic(std::string_view track);
    void playMusic(std::string_view track, Quantize at);
    void stopMusic(float fadeSeconds = 1.0f);
    // A 2D event played on the next beat/bar of the music (right away if no music)
    Handle stinger(std::string_view event, Quantize at = Quantize::Beat, std::initializer_list<Param> params = {});
    MusicInfo music() const;

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
    void snapshots(std::vector<SnapshotInfo>& out) const;
    const Stats& stats() const;
    uint32_t sampleRate() const;
    uint32_t channels() const;
    uint32_t maxVoices() const;

private:
    struct Impl;
    Impl* m = nullptr;
};

} // namespace mkaudio
