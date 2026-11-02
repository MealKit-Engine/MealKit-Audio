#include "mkaudio/Mixer.h"

#include "MiniaudioConfig.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

namespace mkaudio {
namespace {

constexpr uint32_t kVoiceSlots = 256;    // real + virtual + waiting to start
constexpr uint32_t kMaxLayers = 4;       // number of clips a blend container plays together
constexpr size_t kCullHistory = 64;      // record of culled sounds
constexpr float kInaudible = 0.002f;     // below this counts as inaudible (virtual voice)
constexpr uint32_t kMaxStems = 8;        // stems of one music track
constexpr float kOcclusionEase = 0.12f;  // seconds occlusion takes to follow a new amount
constexpr float kOpenCutoff = 20000.0f;  // lowpass "open" (Hz)
constexpr double kLookahead = 0.05;      // seconds ahead music/stinger times are scheduled (the audio thread mixes in chunks)

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Cosmetic randomness (PCG32, unrelated to determinism)
struct Random {
    uint64_t state = 0x853c49e6748fea9bull;
    explicit Random(uint64_t seed = 1) { reseed(seed); }
    void reseed(uint64_t seed) {
        state = 0;
        next();
        state += seed;
        next();
    }
    uint32_t next() {
        const uint64_t old = state;
        state = old * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t shifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (shifted >> rot) | (shifted << ((~rot + 1u) & 31u));
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    float range(float a, float b) { return a + (b - a) * (static_cast<float>(next() >> 8) / 16777216.0f); }
};

// Freeverb (Jezar at Dreampoint, public domain): 8 parallel lowpass-feedback combs + 4 series allpasses per channel
struct Comb {
    std::vector<float> buf;
    size_t pos = 0;
    float store = 0.0f;
    float process(float in, float feedback, float damp) {
        const float out = buf[pos];
        store = out * (1.0f - damp) + store * damp;
        buf[pos] = in + store * feedback;
        if (++pos >= buf.size()) pos = 0;
        return out;
    }
};
struct Allpass {
    std::vector<float> buf;
    size_t pos = 0;
    float process(float in) {
        const float b = buf[pos];
        buf[pos] = in + b * 0.5f;
        if (++pos >= buf.size()) pos = 0;
        return b - in;
    }
};
struct Freeverb {
    Comb comb[2][8];
    Allpass allpass[2][4];
    bool allocated = false;
    void allocate(uint32_t rate) { // tunings are for 44.1 kHz, the right channel is 23 samples longer
        static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int allpasses[4] = {556, 441, 341, 225};
        const double scale = rate / 44100.0;
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < 8; ++i) comb[c][i].buf.assign(static_cast<size_t>((combs[i] + 23 * c) * scale) + 1, 0.0f);
            for (int i = 0; i < 4; ++i) allpass[c][i].buf.assign(static_cast<size_t>((allpasses[i] + 23 * c) * scale) + 1, 0.0f);
        }
        allocated = true;
    }
    void clear() {
        for (int c = 0; c < 2; ++c) {
            for (Comb& k : comb[c]) {
                std::fill(k.buf.begin(), k.buf.end(), 0.0f);
                k.store = 0.0f;
            }
            for (Allpass& a : allpass[c]) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
        }
    }
};

// Node after each bus: lowpass (snapshots) → reverb (buses with reverb) → peak meter.
// The main thread writes the atomics, the audio thread keeps the rest
struct FxNode {
    ma_node_base base;
    std::atomic<float> peak{0.0f};
    std::atomic<float> cutoff{0.0f}; // Hz, 0 = off
    std::atomic<bool> reverbOn{false};
    std::atomic<float> wet{0.0f}, room{0.5f}, damp{0.5f}, width{1.0f};
    uint32_t rate = 48000;
    // audio thread only
    float applied = 0.0f;
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z[2][2] = {};
    float currentWet = 0.0f;
    Freeverb verb;
};

void fxProcess(ma_node* node, const float** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut) {
    FxNode* fx = reinterpret_cast<FxNode*>(node);
    const ma_uint32 channels = ma_node_get_output_channels(node, 0);
    const bool input = framesIn && framesIn[0];
    const ma_uint32 frames = input ? std::min(*frameCountIn, *frameCountOut) : *frameCountOut;
    float* out = framesOut[0];
    if (input) std::copy(framesIn[0], framesIn[0] + frames * channels, out);
    else std::fill(out, out + frames * channels, 0.0f); // nothing connected (a reverb tail still rings)
    const ma_uint32 used = std::min(channels, 2u);

    // Lowpass (2-pole Butterworth, RBJ cookbook)
    const float cutoff = fx->cutoff.load(std::memory_order_relaxed);
    const bool lowpass = cutoff > 0.0f && cutoff < static_cast<float>(fx->rate) * 0.45f;
    if (lowpass && std::fabs(cutoff - fx->applied) > 0.5f) {
        if (fx->applied == 0.0f) std::memset(fx->z, 0, sizeof(fx->z));
        fx->applied = cutoff;
        const float w = 6.2831853f * cutoff / static_cast<float>(fx->rate);
        const float cosw = std::cos(w), alpha = std::sin(w) / (2.0f * 0.70710678f), a0 = 1.0f + alpha;
        fx->b0 = (1.0f - cosw) * 0.5f / a0;
        fx->b1 = (1.0f - cosw) / a0;
        fx->b2 = fx->b0;
        fx->a1 = -2.0f * cosw / a0;
        fx->a2 = (1.0f - alpha) / a0;
    } else if (!lowpass) {
        fx->applied = 0.0f;
    }
    if (lowpass)
        for (ma_uint32 f = 0; f < frames; ++f)
            for (ma_uint32 c = 0; c < used; ++c) { // transposed direct form II
                float& x = out[f * channels + c];
                const float y = fx->b0 * x + fx->z[c][0];
                fx->z[c][0] = fx->b1 * x - fx->a1 * y + fx->z[c][1];
                fx->z[c][1] = fx->b2 * x - fx->a2 * y;
                x = y;
            }

    // Reverb (dry passes as is, wet is added; wet eases across the chunk)
    const bool on = fx->reverbOn.load(std::memory_order_acquire);
    const float targetWet = on ? fx->wet.load(std::memory_order_relaxed) : 0.0f;
    if (on && fx->verb.allocated && frames > 0 && (targetWet > 0.0f || fx->currentWet > 0.0f)) {
        const float feedback = fx->room.load(std::memory_order_relaxed) * 0.28f + 0.7f;
        const float damp = fx->damp.load(std::memory_order_relaxed) * 0.4f;
        const float width = fx->width.load(std::memory_order_relaxed);
        const float startWet = fx->currentWet;
        for (ma_uint32 f = 0; f < frames; ++f) {
            const float w = startWet + (targetWet - startWet) * static_cast<float>(f + 1) / static_cast<float>(frames);
            const float wet1 = w * 3.0f * (width * 0.5f + 0.5f), wet2 = w * 3.0f * ((1.0f - width) * 0.5f);
            float* s = out + f * channels;
            const float in = (s[0] + s[used - 1]) * 0.015f;
            float lr[2] = {0.0f, 0.0f};
            for (int c = 0; c < 2; ++c) {
                for (Comb& k : fx->verb.comb[c]) lr[c] += k.process(in, feedback, damp);
                for (Allpass& a : fx->verb.allpass[c]) lr[c] = a.process(lr[c]);
            }
            if (used == 2) {
                s[0] += lr[0] * wet1 + lr[1] * wet2;
                s[1] += lr[1] * wet1 + lr[0] * wet2;
            } else {
                s[0] += (lr[0] + lr[1]) * 0.5f * (wet1 + wet2);
            }
        }
        fx->currentWet = targetWet;
        if (targetWet == 0.0f) fx->verb.clear(); // finished fading out: an old tail doesn't come back next time
    }

    float peak = fx->peak.load(std::memory_order_relaxed) * 0.97f; // decays slowly
    for (ma_uint32 i = 0; i < frames * channels; ++i) peak = std::max(peak, std::fabs(out[i]));
    fx->peak.store(peak, std::memory_order_relaxed);
    if (input) *frameCountIn = frames;
    *frameCountOut = frames;
}

ma_node_vtable g_fxVtable = {fxProcess, nullptr, 1, 1, MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

struct Bus {
    std::string name;
    ma_sound_group group;
    FxNode fx;
    float volume = 1.0f; // player volume
    float mix = 1.0f;    // mix volume (BusDef)
    uint32_t parent = 0; // master (0) is its own parent
    bool reverb = false;
    std::string duckBy;
    float duckVolume = 0.4f, duckAttack = 0.08f, duckRelease = 0.6f;
    float duck = 1.0f;     // current ducking gain
    float snapshot = 1.0f; // current snapshot volume
    float lowpass = 0.0f;  // current snapshot lowpass
    bool busy = false;     // something sounds on it this frame (ducking trigger)
};

struct SnapshotState {
    SnapshotDef def;
    bool on = false;
    float weight = 0.0f;
};

struct MusicTrack {
    std::string name;
    std::string bus;
    ma_sound stems[kMaxStems];
    float weights[kMaxStems] = {};
    uint32_t stemCount = 0;
    uint64_t start = 0;        // engine frame the track starts at
    uint64_t origin = 0;       // engine frame of beat 0 (start + offset)
    uint64_t end = UINT64_MAX; // engine frame it's silent after (stopping)
};

struct Clip {
    std::string file;
    bool decoded = false;
    ma_sound source; // pre-decoded source (copied to play, only when decoded)
};

struct EventInfo {
    EventDef def;
    std::vector<uint32_t> clips; // m->clips indices (kMissing for missing clips)
    uint32_t bus = 0;
    double lastPlay = -1e9;
    uint32_t lastClip = 0xFFFFFFFFu;
};

constexpr uint32_t kMissing = 0xFFFFFFFFu;

struct Layer {
    ma_sound sound;
    ma_lpf_node lpf;
    bool sound_ = false;
    bool lpf_ = false;
    float cutoff = 0.0f; // the lowpass currently applied (not rebuilt if the same)
};

struct Voice {
    uint32_t generation = 1;
    bool used = false;
    bool pending = true; // waiting for the update() after play()
    bool real = false;   // being mixed
    std::string eventName;
    uint32_t event = 0;
    bool spatial = false;
    Vec3 position;
    std::vector<std::pair<std::string, float>> params;
    float strength = 0.0f; // strength for same-frame grouping (curve parameter, volume if none)
    float volumeScale = 1.0f, pitchScale = 1.0f;
    uint32_t clip = 0; // clip chosen by random/switch (all for blend)
    Layer layers[kMaxLayers];
    uint32_t layerCount = 0;
    double startTime = 0.0;
    double virtualCursor = 0.0; // playback position counted by a virtual voice (seconds)
    double fadeStart = 0.0, fadeEnd = 0.0;
    bool fading = false;
    float audibility = 0.0f;
    float distance = 0.0f;
    float occlusion = 0.0f, occlusionTarget = 0.0f;
    uint64_t startAt = 0; // stinger: engine frame to start at (0 = right away)
};

float lerpf(float a, float b, float t) { return a + (b - a) * t; }

float distanceOf(Vec3 a, Vec3 b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return std::sqrt(x * x + y * y + z * z);
}

} // namespace

struct Mixer::Impl {
    ma_engine engine;
    bool ready = false;
    bool muted = false;
    Desc desc;
    std::vector<std::unique_ptr<Bus>> buses; // [0] = master
    std::vector<std::unique_ptr<Clip>> clips;
    std::vector<EventInfo> events;
    std::unique_ptr<Voice[]> voices;
    std::vector<std::pair<std::string, float>> globals;
    std::deque<CullInfo> culls;
    Stats stats;
    Random rng;
    double time = 0.0;
    std::vector<Subtitle> subtitles;
    Vec3 listener;
    std::vector<SnapshotState> snapshots;
    std::vector<MusicDef> music;
    std::vector<std::unique_ptr<MusicTrack>> tracks; // in start order (the last one not stopping is the current track)
    ReverbParams reverbFrom, reverbNow, reverbTarget;
    float reverbElapsed = 0.0f, reverbSeconds = 0.0f;

    void log(LogLevel level, const std::string& text) const {
        if (desc.log) desc.log(level, text);
    }

    // ---------------------------------------------------------------- Buses
    uint32_t bus(std::string_view name) {
        for (uint32_t i = 0; i < buses.size(); ++i)
            if (buses[i]->name == name) return i;
        auto b = std::make_unique<Bus>();
        b->name = std::string(name);
        // bus → fx (lowpass, reverb, meter) → (master bus → master fx) → device
        ma_sound_group* parent = buses.empty() ? nullptr : &buses[0]->group;
        if (ma_sound_group_init(&engine, 0, parent, &b->group) != MA_SUCCESS) {
            log(LogLevel::Warn, "cannot create bus " + b->name);
            return 0;
        }
        const ma_uint32 channels = ma_engine_get_channels(&engine);
        ma_node_config config = ma_node_config_init();
        config.vtable = &g_fxVtable;
        config.pInputChannels = &channels;
        config.pOutputChannels = &channels;
        b->fx.rate = ma_engine_get_sample_rate(&engine);
        if (ma_node_init(ma_engine_get_node_graph(&engine), &config, nullptr, &b->fx.base) == MA_SUCCESS) {
            ma_node* next = parent ? static_cast<ma_node*>(parent) : ma_engine_get_endpoint(&engine);
            ma_node_attach_output_bus(&b->group, 0, &b->fx, 0);
            ma_node_attach_output_bus(&b->fx, 0, next, 0);
        }
        buses.push_back(std::move(b));
        return static_cast<uint32_t>(buses.size() - 1);
    }

    void applyBusVolume(Bus& b) { ma_sound_group_set_volume(&b.group, b.volume * b.mix * b.duck * b.snapshot); }

    void setBuses(const std::vector<BusDef>& defs) {
        for (const BusDef& d : defs) {
            if (d.name.empty()) continue;
            const uint32_t index = bus(d.name);
            Bus& b = *buses[index];
            b.mix = std::max(d.volume, 0.0f);
            b.duckBy = d.duckBy == d.name ? std::string() : d.duckBy;
            b.duckVolume = std::clamp(d.duckVolume, 0.0f, 1.0f);
            b.duckAttack = std::max(d.duckAttack, 0.0f);
            b.duckRelease = std::max(d.duckRelease, 0.0f);
            if (d.reverb && !b.fx.verb.allocated) b.fx.verb.allocate(b.fx.rate); // before reverbOn (the audio thread reads it after)
            b.reverb = d.reverb;
            b.fx.reverbOn.store(d.reverb, std::memory_order_release);
            applyBusVolume(b);
            if (index == 0) continue; // master is at the top
            uint32_t parent = bus(d.parent.empty() ? "master" : d.parent);
            for (uint32_t p = parent; p != 0; p = buses[p]->parent) // following parents and reaching itself means a cycle
                if (p == index) {
                    log(LogLevel::Warn, "bus " + d.name + ": parent " + d.parent + " makes a loop - using master");
                    parent = 0;
                    break;
                }
            if (parent != b.parent) {
                b.parent = parent;
                ma_node_attach_output_bus(&b.fx, 0, &buses[parent]->group, 0);
            }
        }
    }

    void cull(const std::string& event, const char* reason) {
        culls.push_back({event, reason, time});
        if (culls.size() > kCullHistory) culls.pop_front();
        ++stats.culledThisFrame;
    }

    // ---------------------------------------------------------------- Events/clips
    uint32_t clip(const std::string& file, bool stream) {
        if (file.empty()) return kMissing;
        for (uint32_t i = 0; i < clips.size(); ++i)
            if (clips[i]->file == file) {
                if (!stream && !clips[i]->decoded) decode(*clips[i]);
                return i;
            }
        auto c = std::make_unique<Clip>();
        c->file = file;
        if (!stream) decode(*c);
        clips.push_back(std::move(c));
        return static_cast<uint32_t>(clips.size() - 1);
    }

    // Sound effects are decoded into memory when definitions are received (no disk reads at playback)
    void decode(Clip& c) {
        if (c.decoded) return;
        if (ma_sound_init_from_file(&engine, c.file.c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr,
                                    &c.source) != MA_SUCCESS) {
            log(LogLevel::Warn, "cannot decode " + c.file);
            return;
        }
        c.decoded = true;
    }

    void setEvents(std::vector<EventDef> defs) {
        std::vector<EventInfo> next;
        for (EventDef& d : defs) {
            EventInfo e;
            e.def = std::move(d);
            for (const ClipRef& c : e.def.clips) e.clips.push_back(clip(c.file, e.def.stream));
            e.bus = bus(e.def.bus.empty() ? "sfx" : e.def.bus);
            for (const EventInfo& old : events) // cooldowns/last clips carry over even after a reload
                if (old.def.name == e.def.name) {
                    e.lastPlay = old.lastPlay;
                    e.lastClip = old.lastClip;
                }
            next.push_back(std::move(e));
        }
        events = std::move(next);
        // Playing sounds find their new definitions by name (vanished events stop)
        for (uint32_t i = 0; i < kVoiceSlots; ++i) {
            Voice& v = voices[i];
            if (!v.used) continue;
            const EventInfo* e = find(v.eventName);
            if (e) v.event = static_cast<uint32_t>(e - events.data());
            else release(v);
        }
    }

    const EventInfo* find(std::string_view name) const {
        for (const EventInfo& e : events)
            if (e.def.name == name) return &e;
        return nullptr;
    }

    // ---------------------------------------------------------------- Voices
    float param(const Voice& v, const std::string& name, float fallback) const {
        for (const auto& [n, value] : v.params)
            if (n == name) return value;
        for (const auto& [n, value] : globals)
            if (n == name) return value;
        return fallback;
    }

    // 0..1 position of a curve parameter
    float curveT(const Voice& v, const EventDef& d) const {
        if (d.param.empty()) return 0.0f;
        const float p = param(v, d.param, d.paramMin);
        const float range = d.paramMax - d.paramMin;
        return range != 0.0f ? std::clamp((p - d.paramMin) / range, 0.0f, 1.0f) : 0.0f;
    }

    // Lowpass of a voice: parameter curve and occlusion (the lower one). Nearly open when neither applies
    float cutoffOf(const Voice& v, const EventDef& d, float t) const {
        float c = (d.lowpassAtMin > 0.0f || d.lowpassAtMax > 0.0f) ? lerpf(d.lowpassAtMin, d.lowpassAtMax, t) : 0.0f;
        if (v.spatial && d.occlusionLowpass > 0.0f && v.occlusion > 0.001f) {
            const float o = kOpenCutoff * std::pow(std::min(d.occlusionLowpass, kOpenCutoff) / kOpenCutoff, v.occlusion); // even in octaves
            c = c > 0.0f ? std::min(c, o) : o;
        }
        const float top = static_cast<float>(ma_engine_get_sample_rate(&engine)) * 0.45f;
        return c <= 0.0f ? top : std::clamp(c, 20.0f, top);
    }

    float occlusionGain(const Voice& v, const EventDef& d) const { return v.spatial ? lerpf(1.0f, d.occlusionVolume, v.occlusion) : 1.0f; }

    void unrealize(Voice& v) {
        for (uint32_t i = 0; i < v.layerCount; ++i) {
            Layer& l = v.layers[i];
            if (l.sound_) ma_sound_uninit(&l.sound);
            if (l.lpf_) ma_lpf_node_uninit(&l.lpf, nullptr);
            l.sound_ = l.lpf_ = false;
        }
        v.real = false;
    }

    void release(Voice& v) {
        unrealize(v);
        v.used = false;
        v.params.clear();
        ++v.generation;
    }

    bool realize(Voice& v, double cursor) {
        const EventInfo& e = events[v.event];
        const EventDef& d = e.def;
        // Blend plays all clips together, others the one chosen clip
        std::vector<uint32_t> chosen;
        if (d.container == Container::Blend)
            for (uint32_t i = 0; i < e.clips.size() && i < kMaxLayers; ++i) chosen.push_back(e.clips[i]);
        else if (!e.clips.empty())
            chosen.push_back(e.clips[v.clip % e.clips.size()]);
        v.layerCount = 0;
        for (const uint32_t ci : chosen) {
            if (ci == kMissing) continue;
            Clip& c = *clips[ci];
            Layer& l = v.layers[v.layerCount];
            const bool lowpass = d.lowpassAtMin > 0.0f || d.lowpassAtMax > 0.0f || (v.spatial && d.occlusionLowpass > 0.0f);
            ma_node* target = &buses[e.bus]->group;
            if (lowpass) {
                const ma_uint32 channels = ma_engine_get_channels(&engine);
                const float cutoff = cutoffOf(v, d, curveT(v, d));
                ma_lpf_node_config lc = ma_lpf_node_config_init(channels, ma_engine_get_sample_rate(&engine), cutoff, 2);
                if (ma_lpf_node_init(ma_engine_get_node_graph(&engine), &lc, nullptr, &l.lpf) == MA_SUCCESS) {
                    l.lpf_ = true;
                    l.cutoff = cutoff;
                    ma_node_attach_output_bus(&l.lpf, 0, target, 0);
                }
            }
            ma_result r;
            const ma_uint32 flags = d.spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
            if (c.decoded && !d.stream)
                r = ma_sound_init_copy(&engine, &c.source, flags, nullptr, &l.sound);
            else
                r = ma_sound_init_from_file(&engine, c.file.c_str(), flags | MA_SOUND_FLAG_STREAM, nullptr, nullptr, &l.sound);
            if (r != MA_SUCCESS) {
                if (l.lpf_) ma_lpf_node_uninit(&l.lpf, nullptr);
                l.lpf_ = false;
                continue;
            }
            l.sound_ = true;
            ma_node_attach_output_bus(&l.sound, 0, l.lpf_ ? static_cast<ma_node*>(&l.lpf) : target, 0);
            ma_sound_set_looping(&l.sound, d.loop);
            if (d.spatial) {
                ma_sound_set_attenuation_model(&l.sound, ma_attenuation_model_linear);
                ma_sound_set_min_distance(&l.sound, d.minDistance);
                ma_sound_set_max_distance(&l.sound, std::max(d.maxDistance, d.minDistance + 0.01f));
            }
            if (cursor > 0.0) { // a loop returning from virtual: continue from the counted position
                ma_uint64 length = 0;
                ma_sound_get_length_in_pcm_frames(&l.sound, &length);
                const ma_uint32 rate = ma_engine_get_sample_rate(&engine);
                if (length > 0) ma_sound_seek_to_pcm_frame(&l.sound, static_cast<ma_uint64>(cursor * rate) % length);
            }
            ++v.layerCount;
        }
        if (v.layerCount == 0) return false;
        v.real = true;
        apply(v);
        const bool later = v.startAt > ma_engine_get_time_in_pcm_frames(&engine);
        for (uint32_t i = 0; i < v.layerCount; ++i) {
            if (later) ma_sound_set_start_time_in_pcm_frames(&v.layers[i].sound, v.startAt);
            ma_sound_start(&v.layers[i].sound);
        }
        return true;
    }

    // Puts parameters/position/fades into the real voice
    void apply(Voice& v) {
        const EventInfo& e = events[v.event];
        const EventDef& d = e.def;
        const float t = curveT(v, d);
        float volume = d.volume * v.volumeScale * lerpf(d.volumeAtMin, d.volumeAtMax, t) * occlusionGain(v, d);
        if (v.fading)
            volume *= static_cast<float>(std::clamp((v.fadeEnd - time) / std::max(v.fadeEnd - v.fadeStart, 1e-4), 0.0, 1.0));
        const float pitch = std::max(0.01f, d.pitch * v.pitchScale * lerpf(d.pitchAtMin, d.pitchAtMax, t));
        const float cutoff = cutoffOf(v, d, t);
        for (uint32_t i = 0; i < v.layerCount; ++i) {
            Layer& l = v.layers[i];
            float weight = 1.0f;
            if (d.container == Container::Blend && v.layerCount > 1) { // crossfades two adjacent clips
                const float at = (d.param.empty() ? 0.0f : t) * static_cast<float>(v.layerCount - 1);
                weight = std::max(0.0f, 1.0f - std::fabs(at - static_cast<float>(i)));
            }
            ma_sound_set_volume(&l.sound, volume * weight);
            ma_sound_set_pitch(&l.sound, pitch);
            if (d.spatial) ma_sound_set_position(&l.sound, v.position.x, v.position.y, -v.position.z); // left-handed → miniaudio right-handed
            if (l.lpf_ && std::fabs(cutoff - l.cutoff) > 1.0f) {
                l.cutoff = cutoff;
                ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&engine), ma_engine_get_sample_rate(&engine), cutoff, 2);
                ma_lpf_node_reinit(&lc, &l.lpf);
            }
        }
    }

    float estimate(Voice& v) const { // volume × distance attenuation (virtual voice/limit decisions)
        const EventDef& d = events[v.event].def;
        const float t = curveT(v, d);
        float gain = d.volume * v.volumeScale * lerpf(d.volumeAtMin, d.volumeAtMax, t) * occlusionGain(v, d);
        v.distance = 0.0f;
        if (d.spatial) {
            v.distance = distanceOf(v.position, listener);
            const float range = std::max(d.maxDistance - d.minDistance, 0.01f);
            gain *= std::clamp(1.0f - (v.distance - d.minDistance) / range, 0.0f, 1.0f);
        }
        return gain;
    }

    bool finished(const Voice& v) const {
        if (!v.real) return false;
        for (uint32_t i = 0; i < v.layerCount; ++i)
            if (!ma_sound_at_end(&v.layers[i].sound)) return false;
        return true;
    }

    void start(Voice& v) {
        EventInfo& e = events[v.event];
        const EventDef& d = e.def;
        v.pending = false;
        v.startTime = time;
        v.volumeScale = 1.0f + (d.volumeRandom > 0.0f ? rng.range(-d.volumeRandom, d.volumeRandom) : 0.0f);
        v.pitchScale = 1.0f + (d.pitchRandom > 0.0f ? rng.range(-d.pitchRandom, d.pitchRandom) : 0.0f);
        const uint32_t n = static_cast<uint32_t>(e.clips.size());
        if (d.container == Container::Switch && n > 0) {
            const float p = param(v, d.param, 0.0f);
            v.clip = static_cast<uint32_t>(std::clamp(std::lround(p), 0l, static_cast<long>(n) - 1));
        } else if (n > 1) { // random: the last clip isn't picked again
            v.clip = rng.below(n - 1);
            if (v.clip >= e.lastClip && e.lastClip < n) ++v.clip;
        } else {
            v.clip = 0;
        }
        e.lastClip = v.clip;
        e.lastPlay = time;
        ++stats.playsThisFrame;
    }

    Handle handleOf(uint32_t index) const { return {index, voices[index].generation}; }
    Voice* voiceOf(Handle h) const {
        if (!voices || !h.valid() || h.index >= kVoiceSlots) return nullptr;
        Voice& v = voices[h.index];
        return v.used && v.generation == h.generation ? &v : nullptr;
    }

    Handle request(std::string_view name, bool spatial, Vec3 position, std::initializer_list<Param> params) {
        if (!ready) return {};
        const EventInfo* e = find(name);
        if (!e) {
            cull(std::string(name), "missing event");
            return {};
        }
        if (!e->def.subtitle.empty()) { // subtitles: same keys on one line
            auto at = std::find_if(subtitles.begin(), subtitles.end(), [&](const Subtitle& s) { return s.key == e->def.subtitle; });
            if (at != subtitles.end()) at->secondsLeft = std::max(at->secondsLeft, e->def.subtitleSeconds);
            else subtitles.push_back({e->def.subtitle, e->def.subtitleSeconds});
        }
        for (uint32_t i = 0; i < kVoiceSlots; ++i) {
            Voice& v = voices[i];
            if (v.used) continue;
            v.used = true;
            v.pending = true;
            v.real = v.fading = false;
            v.eventName = e->def.name;
            v.event = static_cast<uint32_t>(e - events.data());
            v.spatial = spatial && e->def.spatial;
            v.position = position;
            v.params.clear();
            for (const Param& p : params) v.params.emplace_back(p.name, p.value);
            v.strength = e->def.param.empty() ? e->def.volume : param(v, e->def.param, e->def.paramMin);
            v.layerCount = 0;
            v.virtualCursor = 0.0;
            v.occlusion = v.occlusionTarget = 0.0f;
            v.startAt = 0;
            return handleOf(i);
        }
        cull(e->def.name, "voice limit");
        return {};
    }

    // Requests waiting to start: per-event cooldown, concurrency limit (what to cut on overflow), same-frame grouping (loudest first)
    void startPending() {
        std::vector<uint32_t> pending;
        for (uint32_t i = 0; i < kVoiceSlots; ++i)
            if (voices[i].used && voices[i].pending) pending.push_back(i);
        std::sort(pending.begin(), pending.end(), [&](uint32_t a, uint32_t b) {
            const Voice &va = voices[a], &vb = voices[b];
            if (va.event != vb.event) return va.event < vb.event;
            if (va.strength != vb.strength) return va.strength > vb.strength;
            return a < b;
        });
        for (size_t at = 0; at < pending.size();) {
            const uint32_t event = voices[pending[at]].event;
            size_t end = at;
            while (end < pending.size() && voices[pending[end]].event == event) ++end;
            EventInfo& e = events[event];
            const EventDef& d = e.def;
            const bool cooling = d.cooldown > 0.0f && time - e.lastPlay < d.cooldown;
            uint32_t active = 0;
            for (uint32_t i = 0; i < kVoiceSlots; ++i)
                if (voices[i].used && !voices[i].pending && voices[i].event == event) ++active;
            const uint32_t cap = std::max(d.maxInstances, 1u);
            for (size_t k = at; k < end; ++k) {
                Voice& v = voices[pending[k]];
                if (cooling) {
                    cull(d.name, "cooldown");
                    release(v);
                    continue;
                }
                if (k - at >= cap) { // more than the limit arrived in one frame: only the loudest
                    cull(d.name, "same frame");
                    release(v);
                    continue;
                }
                if (active >= cap) {
                    Voice* victim = nullptr;
                    if (d.steal != Steal::None)
                        for (uint32_t i = 0; i < kVoiceSlots; ++i) {
                            Voice& o = voices[i];
                            if (!o.used || o.pending || o.event != event) continue;
                            if (!victim ||
                                (d.steal == Steal::Oldest ? o.startTime < victim->startTime : o.audibility < victim->audibility))
                                victim = &o;
                        }
                    if (!victim) {
                        cull(d.name, "max instances");
                        release(v);
                        continue;
                    }
                    cull(d.name, "max instances (stolen)");
                    release(*victim);
                    --active;
                }
                start(v);
                ++active;
            }
            at = end;
        }
    }

    // Picking voices to actually mix: priority × audibility. Other loops become virtual (only the playback position is counted), short sounds are cut
    void manageVoices(double dt) {
        std::vector<uint32_t> active;
        for (uint32_t i = 0; i < kVoiceSlots; ++i) {
            Voice& v = voices[i];
            if (!v.used || v.pending) continue;
            if (finished(v) || (v.fading && time >= v.fadeEnd)) {
                release(v);
                continue;
            }
            const float step = static_cast<float>(dt) / kOcclusionEase;
            v.occlusion += std::clamp(v.occlusionTarget - v.occlusion, -step, step);
            v.audibility = estimate(v);
            if (!v.real) v.virtualCursor += dt * events[v.event].def.pitch;
            active.push_back(i);
        }
        std::sort(active.begin(), active.end(), [&](uint32_t a, uint32_t b) {
            const Voice &va = voices[a], &vb = voices[b];
            const float sa = static_cast<float>(events[va.event].def.priority + 1) * va.audibility;
            const float sb = static_cast<float>(events[vb.event].def.priority + 1) * vb.audibility;
            if (sa != sb) return sa > sb;
            return va.startTime > vb.startTime;
        });
        uint32_t real = 0;
        stats.realVoices = stats.virtualVoices = 0;
        for (const uint32_t i : active) {
            Voice& v = voices[i];
            const bool loop = events[v.event].def.loop;
            const bool audible = v.audibility >= kInaudible && real < desc.maxVoices;
            if (audible) {
                if (!v.real) {
                    // First start, or returning from virtual (short sounds come here only on first start)
                    if (!realize(v, loop ? v.virtualCursor : 0.0)) {
                        cull(v.eventName, "missing clip");
                        release(v);
                        continue;
                    }
                } else {
                    apply(v);
                }
                ++real;
                ++stats.realVoices;
                continue;
            }
            if (!loop) { // short sounds aren't kept virtual
                cull(v.eventName, v.audibility < kInaudible ? "inaudible" : "voice limit");
                release(v);
                continue;
            }
            if (v.real) {
                ma_uint64 cursor = 0;
                ma_sound_get_cursor_in_pcm_frames(&v.layers[0].sound, &cursor);
                v.virtualCursor = static_cast<double>(cursor) / ma_engine_get_sample_rate(&engine);
                unrealize(v);
            }
            ++stats.virtualVoices;
        }
    }

    // ---------------------------------------------------------------- Mix: snapshots, ducking, reverb
    void updateMix(double dt) {
        const float fdt = static_cast<float>(dt);
        for (SnapshotState& s : snapshots) {
            const float step = s.def.fade > 0.0f ? fdt / s.def.fade : 1.0f;
            s.weight = std::clamp(s.weight + (s.on ? step : -step), 0.0f, 1.0f);
        }
        // What sounds this frame (a voice or a track marks its bus and the parents)
        for (auto& b : buses) b->busy = false;
        const auto mark = [&](uint32_t index) {
            for (uint32_t p = index;; p = buses[p]->parent) {
                buses[p]->busy = true;
                if (p == 0) break;
            }
        };
        const uint64_t frame = engineFrame();
        for (uint32_t i = 0; i < kVoiceSlots; ++i) {
            const Voice& v = voices[i];
            if (v.used && v.real && v.audibility >= kInaudible && v.startAt <= frame) mark(events[v.event].bus);
        }
        for (const auto& t : tracks)
            if (t->start <= frame && frame < t->end) mark(bus(t->bus));
        // Reverb eases toward its target
        reverbElapsed += fdt;
        const float k = reverbSeconds > 0.0f ? std::min(reverbElapsed / reverbSeconds, 1.0f) : 1.0f;
        reverbNow = {lerpf(reverbFrom.roomSize, reverbTarget.roomSize, k), lerpf(reverbFrom.damping, reverbTarget.damping, k),
                     lerpf(reverbFrom.wet, reverbTarget.wet, k), lerpf(reverbFrom.width, reverbTarget.width, k)};
        for (auto& bp : buses) {
            Bus& b = *bp;
            float volume = 1.0f, lowpass = 0.0f;
            for (const SnapshotState& s : snapshots) {
                if (s.weight <= 0.0f) continue;
                for (const SnapshotBus& sb : s.def.buses) {
                    if (sb.bus != b.name) continue;
                    volume *= lerpf(1.0f, std::max(sb.volume, 0.0f), s.weight);
                    if (sb.lowpass > 0.0f) {
                        const float c = kOpenCutoff * std::pow(std::min(sb.lowpass, kOpenCutoff) / kOpenCutoff, s.weight);
                        lowpass = lowpass > 0.0f ? std::min(lowpass, c) : c;
                    }
                }
            }
            b.snapshot = volume;
            b.lowpass = lowpass >= kOpenCutoff * 0.99f ? 0.0f : lowpass;
            b.fx.cutoff.store(b.lowpass, std::memory_order_relaxed);
            bool ducked = false;
            if (!b.duckBy.empty())
                for (const auto& o : buses)
                    if (o->name == b.duckBy) ducked = o->busy;
            const float depth = 1.0f - b.duckVolume;
            if (ducked) b.duck = b.duckAttack > 0.0f ? std::max(b.duckVolume, b.duck - depth * fdt / b.duckAttack) : b.duckVolume;
            else b.duck = b.duckRelease > 0.0f ? std::min(1.0f, b.duck + depth * fdt / b.duckRelease) : 1.0f;
            applyBusVolume(b);
            if (b.reverb) {
                b.fx.wet.store(std::clamp(reverbNow.wet, 0.0f, 1.0f), std::memory_order_relaxed);
                b.fx.room.store(std::clamp(reverbNow.roomSize, 0.0f, 1.0f), std::memory_order_relaxed);
                b.fx.damp.store(std::clamp(reverbNow.damping, 0.0f, 1.0f), std::memory_order_relaxed);
                b.fx.width.store(std::clamp(reverbNow.width, 0.0f, 1.0f), std::memory_order_relaxed);
            }
        }
    }

    // ---------------------------------------------------------------- Music
    uint64_t engineFrame() const { return ma_engine_get_time_in_pcm_frames(&engine); }
    uint64_t lookahead() const { return static_cast<uint64_t>(kLookahead * ma_engine_get_sample_rate(&engine)); }
    uint64_t frames(float seconds) const { return static_cast<uint64_t>(std::max(seconds, 0.0f) * ma_engine_get_sample_rate(&engine)); }
    const MusicDef* findMusic(std::string_view name) const {
        for (const MusicDef& d : music)
            if (d.name == name) return &d;
        return nullptr;
    }
    MusicTrack* current() const { // the last track not stopping
        for (size_t i = tracks.size(); i-- > 0;)
            if (tracks[i]->end == UINT64_MAX) return tracks[i].get();
        return nullptr;
    }
    double beatFrames(const MusicDef& d) const { return 60.0 / std::max(d.bpm, 1.0f) * ma_engine_get_sample_rate(&engine); }

    // Next beat/bar of a track after now (+ lookahead)
    uint64_t boundary(const MusicTrack& t, Quantize at) const {
        const uint64_t soon = engineFrame() + lookahead();
        const MusicDef* d = findMusic(t.name);
        if (at == Quantize::Immediate || !d) return soon;
        if (soon <= t.origin) return t.origin;
        const double unit = beatFrames(*d) * (at == Quantize::Bar ? std::max(d->beatsPerBar, 1u) : 1u);
        const double n = std::ceil(static_cast<double>(soon - t.origin) / unit);
        return t.origin + static_cast<uint64_t>(std::llround(n * unit));
    }

    float stemWeight(const MusicDef& d, uint32_t stem, uint32_t count) const {
        if (stem == 0 || d.stemParam.empty() || count < 2) return 1.0f;
        float p = d.paramMin;
        for (const auto& [n, value] : globals)
            if (n == d.stemParam) p = value;
        const float range = d.paramMax - d.paramMin;
        const float t = range != 0.0f ? std::clamp((p - d.paramMin) / range, 0.0f, 1.0f) : 0.0f;
        return std::clamp(t * static_cast<float>(count - 1) - static_cast<float>(stem - 1), 0.0f, 1.0f);
    }

    void stopTrack(MusicTrack& t, uint64_t at, uint64_t fade) {
        for (uint32_t i = 0; i < t.stemCount; ++i) ma_sound_set_stop_time_with_fade_in_pcm_frames(&t.stems[i], at + fade, fade);
        t.end = at + fade;
    }

    void freeTrack(MusicTrack& t) {
        for (uint32_t i = 0; i < t.stemCount; ++i) ma_sound_uninit(&t.stems[i]);
        t.stemCount = 0;
    }

    void startTrack(const MusicDef& d, uint64_t at) {
        auto t = std::make_unique<MusicTrack>();
        t->name = d.name;
        t->bus = d.bus.empty() ? "music" : d.bus;
        t->start = at;
        t->origin = at + frames(d.offset);
        ma_node* target = &buses[bus(t->bus)]->group;
        const uint64_t fade = frames(d.crossfade);
        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(d.stems.size(), kMaxStems));
        for (uint32_t i = 0; i < count; ++i) {
            ma_sound& s = t->stems[t->stemCount];
            if (d.stems[i].file.empty() ||
                ma_sound_init_from_file(&engine, d.stems[i].file.c_str(), MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr,
                                        &s) != MA_SUCCESS) {
                log(LogLevel::Warn, "music " + d.name + ": cannot open stem " + d.stems[i].name);
                continue;
            }
            ma_node_attach_output_bus(&s, 0, target, 0);
            ma_sound_set_looping(&s, d.loop);
            t->weights[t->stemCount] = stemWeight(d, i, count);
            ma_sound_set_volume(&s, d.volume * t->weights[t->stemCount]);
            ma_sound_set_start_time_in_pcm_frames(&s, at);
            if (fade > 0) ma_sound_set_fade_start_in_pcm_frames(&s, 0.0f, 1.0f, fade, at);
            ma_sound_start(&s);
            ++t->stemCount;
        }
        if (t->stemCount == 0) {
            cull(d.name, "missing clip");
            return;
        }
        tracks.push_back(std::move(t));
    }

    void playMusic(std::string_view name, const Quantize* at) {
        const MusicDef* d = findMusic(name);
        if (!d) {
            cull(std::string(name), "missing music");
            return;
        }
        MusicTrack* cur = current();
        if (cur && cur->name == name) return;
        uint64_t when = engineFrame() + lookahead();
        if (cur && cur->start > engineFrame()) { // the current one hasn't started yet: this one takes its place
            when = cur->start;
            freeTrack(*cur);
            std::erase_if(tracks, [&](const auto& t) { return t.get() == cur; });
        } else if (cur) {
            when = boundary(*cur, at ? *at : d->transition);
            stopTrack(*cur, when, frames(d->crossfade));
        }
        startTrack(*d, when);
    }

    void stopMusic(float fadeSeconds) {
        const uint64_t frame = engineFrame();
        for (auto& t : tracks) {
            if (t->end != UINT64_MAX) continue;
            if (t->start > frame) freeTrack(*t); // not started yet
            else stopTrack(*t, frame + lookahead(), frames(fadeSeconds));
        }
        std::erase_if(tracks, [](const auto& t) { return t->stemCount == 0; });
    }

    void updateMusic(double dt) {
        const uint64_t frame = engineFrame();
        for (auto& t : tracks) {
            const MusicDef* d = findMusic(t->name);
            if (!d) { // the definition vanished
                if (t->end == UINT64_MAX) stopTrack(*t, frame, lookahead());
                continue;
            }
            const float step = d->stemFade > 0.0f ? static_cast<float>(dt) / d->stemFade : 1.0f;
            for (uint32_t i = 0; i < t->stemCount; ++i) {
                t->weights[i] += std::clamp(stemWeight(*d, i, t->stemCount) - t->weights[i], -step, step);
                ma_sound_set_volume(&t->stems[i], d->volume * t->weights[i]);
            }
        }
        for (auto& t : tracks) // stopped tracks are freed a little after their end (the audio thread may still be mixing their last chunk)
            if (t->end != UINT64_MAX && frame > t->end + lookahead()) freeTrack(*t);
        std::erase_if(tracks, [](const auto& t) { return t->stemCount == 0; });
    }
};

Mixer::Mixer() : m(new Impl()) {}

Mixer::~Mixer() {
    shutdown();
    delete m;
}

bool Mixer::init(const Desc& desc) {
    shutdown();
    m->desc = desc;
    m->desc.maxVoices = std::clamp(desc.maxVoices, 1u, kVoiceSlots);
    ma_engine_config config = ma_engine_config_init();
    config.listenerCount = 1;
    config.channels = 2; // mixed in stereo, miniaudio converts to device channels (7.1 etc.) (mixing cost scales with channel count)
    if (desc.noDevice) {
        config.noDevice = MA_TRUE;
        config.sampleRate = desc.sampleRate;
    }
    if (ma_engine_init(&config, &m->engine) != MA_SUCCESS) {
        m->log(LogLevel::Warn, desc.noDevice ? "cannot start the mixer" : "no output device - running without sound");
        return false;
    }
    m->ready = true;
    m->voices = std::make_unique<Voice[]>(kVoiceSlots);
    m->bus("master");
    for (const char* name : {"music", "sfx", "ui", "voice"}) m->bus(name);
    m->rng.reseed(desc.seed ? desc.seed : static_cast<uint64_t>(now() * 1000.0) | 1u); // cosmetic (unrelated to determinism)
    setMuted(desc.muted);
    return true;
}

void Mixer::shutdown() {
    if (!m->ready) return;
    for (uint32_t i = 0; i < kVoiceSlots; ++i)
        if (m->voices[i].used) m->release(m->voices[i]);
    for (auto& t : m->tracks) m->freeTrack(*t);
    m->tracks.clear();
    for (auto& c : m->clips)
        if (c->decoded) ma_sound_uninit(&c->source);
    m->clips.clear();
    for (size_t i = m->buses.size(); i-- > 0;) { // child buses first
        ma_node_uninit(&m->buses[i]->fx.base, nullptr);
        ma_sound_group_uninit(&m->buses[i]->group);
    }
    m->buses.clear();
    m->events.clear();
    ma_engine_uninit(&m->engine);
    m->voices.reset();
    m->ready = false;
}

bool Mixer::ready() const { return m->ready; }

void Mixer::setEvents(std::vector<EventDef> events) {
    if (m->ready) m->setEvents(std::move(events));
}

void Mixer::setBuses(const std::vector<BusDef>& buses) {
    if (m->ready) m->setBuses(buses);
}

Handle Mixer::play(std::string_view event, Vec3 position, std::initializer_list<Param> params) {
    return m->request(event, true, position, params);
}

Handle Mixer::play2D(std::string_view event, std::initializer_list<Param> params) { return m->request(event, false, {}, params); }

void Mixer::setParam(Handle h, std::string_view name, float value) {
    Voice* v = m->voiceOf(h);
    if (!v) return;
    for (auto& [n, val] : v->params)
        if (n == name) {
            val = value;
            return;
        }
    v->params.emplace_back(std::string(name), value);
}

void Mixer::setPosition(Handle h, Vec3 position) {
    if (Voice* v = m->voiceOf(h)) v->position = position;
}

void Mixer::stop(Handle h, float fadeSeconds) {
    Voice* v = m->voiceOf(h);
    if (!v) return;
    if (fadeSeconds <= 0.0f || !v->real) {
        m->release(*v);
        return;
    }
    v->fading = true;
    v->fadeStart = m->time;
    v->fadeEnd = m->time + fadeSeconds;
}

void Mixer::stopAll() {
    if (!m->ready) return;
    for (uint32_t i = 0; i < kVoiceSlots; ++i)
        if (m->voices[i].used) m->release(m->voices[i]);
}

bool Mixer::playing(Handle h) const { return m->voiceOf(h) != nullptr; }

void Mixer::setGlobalParam(std::string_view name, float value) {
    for (auto& [n, v] : m->globals)
        if (n == name) {
            v = value;
            return;
        }
    m->globals.emplace_back(std::string(name), value);
}

void Mixer::setListener(Vec3 position, Vec3 forward) {
    if (!m->ready) return;
    m->listener = position;
    ma_engine_listener_set_position(&m->engine, 0, position.x, position.y, -position.z); // left-handed coordinates → miniaudio right-handed
    ma_engine_listener_set_direction(&m->engine, 0, forward.x, forward.y, -forward.z);
    ma_engine_listener_set_world_up(&m->engine, 0, 0.0f, 1.0f, 0.0f);
}

void Mixer::setBusVolume(std::string_view name, float volume) {
    if (!m->ready) return;
    Bus& b = *m->buses[m->bus(name)];
    b.volume = std::max(volume, 0.0f);
    m->applyBusVolume(b);
}

float Mixer::busVolume(std::string_view name) const {
    for (const auto& b : m->buses)
        if (b->name == name) return b->volume;
    return 1.0f;
}

void Mixer::setMuted(bool muted) {
    m->muted = muted;
    if (m->ready) ma_engine_set_volume(&m->engine, muted ? 0.0f : 1.0f);
}

bool Mixer::muted() const { return m->muted; }

const std::vector<Subtitle>& Mixer::subtitles() const { return m->subtitles; }

void Mixer::setSnapshots(std::vector<SnapshotDef> snapshots) {
    std::vector<SnapshotState> next;
    for (SnapshotDef& d : snapshots) next.push_back({std::move(d), false, 0.0f});
    for (const SnapshotState& old : m->snapshots) { // on/fade carry over by name (also ones turned on before they were defined)
        bool found = false;
        for (SnapshotState& s : next)
            if (s.def.name == old.def.name) {
                s.on = old.on;
                s.weight = old.weight;
                found = true;
            }
        if (!found && old.on) next.push_back({{old.def.name, old.def.fade, {}}, true, old.weight});
    }
    m->snapshots = std::move(next);
}

void Mixer::setMusic(std::vector<MusicDef> tracks) { m->music = std::move(tracks); }

void Mixer::emitters(std::vector<Emitter>& out) const {
    out.clear();
    if (!m->ready) return;
    for (uint32_t i = 0; i < kVoiceSlots; ++i) {
        const Voice& v = m->voices[i];
        if (v.used && v.real && v.spatial) out.push_back({m->handleOf(i), v.position, v.audibility, v.occlusionTarget});
    }
}

void Mixer::setOcclusion(Handle h, float amount) {
    if (Voice* v = m->voiceOf(h)) v->occlusionTarget = std::clamp(amount, 0.0f, 1.0f);
}

void Mixer::setReverb(const ReverbParams& reverb, float seconds) {
    m->reverbFrom = m->reverbNow;
    m->reverbTarget = reverb;
    m->reverbElapsed = 0.0f;
    m->reverbSeconds = std::max(seconds, 0.0f);
}

const ReverbParams& Mixer::reverb() const { return m->reverbTarget; }

void Mixer::setSnapshot(std::string_view name, bool on) {
    for (SnapshotState& s : m->snapshots)
        if (s.def.name == name) {
            s.on = on;
            return;
        }
    if (on) m->snapshots.push_back({{std::string(name), 0.3f, {}}, true, 0.0f}); // not defined (yet): kept by name
}

bool Mixer::snapshot(std::string_view name) const {
    for (const SnapshotState& s : m->snapshots)
        if (s.def.name == name) return s.on;
    return false;
}

void Mixer::playMusic(std::string_view track) {
    if (m->ready) m->playMusic(track, nullptr);
}

void Mixer::playMusic(std::string_view track, Quantize at) {
    if (m->ready) m->playMusic(track, &at);
}

void Mixer::stopMusic(float fadeSeconds) {
    if (m->ready) m->stopMusic(fadeSeconds);
}

Handle Mixer::stinger(std::string_view event, Quantize at, std::initializer_list<Param> params) {
    const Handle h = m->request(event, false, {}, params);
    Voice* v = m->voiceOf(h);
    const MusicTrack* cur = m->current();
    if (v && cur && at != Quantize::Immediate) v->startAt = m->boundary(*cur, at);
    return h;
}

MusicInfo Mixer::music() const {
    MusicInfo info;
    if (!m->ready) return info;
    const uint64_t frame = m->engineFrame();
    const MusicTrack* playing = nullptr;
    for (const auto& t : m->tracks) {
        if (t->start <= frame && frame < t->end) playing = t.get();
        else if (t->start > frame && t->end == UINT64_MAX) info.next = t->name;
    }
    if (!playing) {
        std::swap(info.track, info.next); // only a track about to start
        return info;
    }
    info.track = playing->name;
    if (const MusicDef* d = m->findMusic(playing->name)) {
        info.bpm = d->bpm;
        info.beatsPerBar = std::max(d->beatsPerBar, 1u);
        const double beats = frame > playing->origin ? static_cast<double>(frame - playing->origin) / m->beatFrames(*d) : 0.0;
        info.bar = static_cast<uint32_t>(beats / info.beatsPerBar);
        info.beat = static_cast<float>(std::fmod(beats, static_cast<double>(info.beatsPerBar)));
    }
    return info;
}

void Mixer::update(double dt) {
    if (!m->ready) return;
    const double begin = now();
    m->time += dt;
    for (Subtitle& s : m->subtitles) s.secondsLeft -= static_cast<float>(dt);
    std::erase_if(m->subtitles, [](const Subtitle& s) { return s.secondsLeft <= 0.0f; });
    m->stats.playsThisFrame = m->stats.culledThisFrame = 0;
    m->startPending();
    m->manageVoices(dt);
    m->updateMusic(dt);
    m->updateMix(dt);
    m->stats.updateMs = (now() - begin) * 1000.0;
}

uint64_t Mixer::render(float* out, uint64_t frames) {
    if (!m->ready || !m->desc.noDevice) return 0;
    ma_uint64 read = 0;
    if (ma_engine_read_pcm_frames(&m->engine, out, frames, &read) != MA_SUCCESS) return 0;
    return read;
}

std::vector<std::string> Mixer::eventNames() const {
    std::vector<std::string> names;
    for (const EventInfo& e : m->events) names.push_back(e.def.name);
    return names;
}

const EventDef* Mixer::event(std::string_view name) const {
    const EventInfo* e = m->find(name);
    return e ? &e->def : nullptr;
}

void Mixer::voices(std::vector<VoiceInfo>& out) const {
    out.clear();
    if (!m->ready) return;
    for (uint32_t i = 0; i < kVoiceSlots; ++i) {
        const Voice& v = m->voices[i];
        if (!v.used || v.pending) continue;
        out.push_back({v.eventName, !v.real, m->events[v.event].def.loop, v.audibility, v.distance, m->time - v.startTime, v.occlusion});
    }
}

const std::deque<CullInfo>& Mixer::culls() const { return m->culls; }

void Mixer::buses(std::vector<BusInfo>& out) const {
    out.clear();
    for (size_t i = 0; i < m->buses.size(); ++i) {
        const Bus& b = *m->buses[i];
        out.push_back({b.name, b.volume, b.fx.peak.load(std::memory_order_relaxed), b.mix, i ? m->buses[b.parent]->name : std::string(), b.duck,
                       b.snapshot, b.lowpass, b.reverb});
    }
}

void Mixer::snapshots(std::vector<SnapshotInfo>& out) const {
    out.clear();
    for (const SnapshotState& s : m->snapshots) out.push_back({s.def.name, s.on, s.weight});
}

const Stats& Mixer::stats() const { return m->stats; }

uint32_t Mixer::sampleRate() const { return m->ready ? ma_engine_get_sample_rate(&m->engine) : 0; }

uint32_t Mixer::channels() const { return m->ready ? ma_engine_get_channels(&m->engine) : 0; }

uint32_t Mixer::maxVoices() const { return m->desc.maxVoices; }

} // namespace mkaudio
