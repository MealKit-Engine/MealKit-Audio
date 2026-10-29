#include "mkaudio/Mixer.h"

#include "MiniaudioConfig.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>

namespace mkaudio {
namespace {

constexpr uint32_t kVoiceSlots = 256;    // real + virtual + waiting to start
constexpr uint32_t kMaxLayers = 4;       // number of clips a blend container plays together
constexpr size_t kCullHistory = 64;      // record of culled sounds
constexpr float kInaudible = 0.002f;     // below this counts as inaudible (virtual voice)

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

// Node that measures the peak of bus output (written by the audio thread, read by the main thread)
struct MeterNode {
    ma_node_base base;
    std::atomic<float> peak{0.0f};
};

void meterProcess(ma_node* node, const float** framesIn, ma_uint32* frameCountIn, float** framesOut, ma_uint32* frameCountOut) {
    MeterNode* meter = reinterpret_cast<MeterNode*>(node);
    const ma_uint32 channels = ma_node_get_output_channels(node, 0);
    if (!framesIn || !framesIn[0]) { // nothing is connected
        std::fill(framesOut[0], framesOut[0] + *frameCountOut * channels, 0.0f);
        return;
    }
    const ma_uint32 frames = std::min(*frameCountIn, *frameCountOut);
    float peak = meter->peak.load(std::memory_order_relaxed) * 0.97f; // decays slowly
    const ma_uint32 samples = frames * channels;
    for (ma_uint32 i = 0; i < samples; ++i) {
        const float s = framesIn[0][i];
        framesOut[0][i] = s;
        peak = std::max(peak, std::fabs(s));
    }
    *frameCountIn = frames;
    *frameCountOut = frames;
    meter->peak.store(peak, std::memory_order_relaxed);
}

ma_node_vtable g_meterVtable = {meterProcess, nullptr, 1, 1, 0};

struct Bus {
    std::string name;
    ma_sound_group group;
    MeterNode meter;
    float volume = 1.0f; // player volume
    float mix = 1.0f;    // mix volume (BusDef)
    uint32_t parent = 0; // master (0) is its own parent
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

    void log(LogLevel level, const std::string& text) const {
        if (desc.log) desc.log(level, text);
    }

    // ---------------------------------------------------------------- Buses
    uint32_t bus(std::string_view name) {
        for (uint32_t i = 0; i < buses.size(); ++i)
            if (buses[i]->name == name) return i;
        auto b = std::make_unique<Bus>();
        b->name = std::string(name);
        // bus → meter → (master bus → master meter) → device
        ma_sound_group* parent = buses.empty() ? nullptr : &buses[0]->group;
        if (ma_sound_group_init(&engine, 0, parent, &b->group) != MA_SUCCESS) {
            log(LogLevel::Warn, "cannot create bus " + b->name);
            return 0;
        }
        const ma_uint32 channels = ma_engine_get_channels(&engine);
        ma_node_config config = ma_node_config_init();
        config.vtable = &g_meterVtable;
        config.pInputChannels = &channels;
        config.pOutputChannels = &channels;
        if (ma_node_init(ma_engine_get_node_graph(&engine), &config, nullptr, &b->meter.base) == MA_SUCCESS) {
            ma_node* next = parent ? static_cast<ma_node*>(parent) : ma_engine_get_endpoint(&engine);
            ma_node_attach_output_bus(&b->group, 0, &b->meter, 0);
            ma_node_attach_output_bus(&b->meter, 0, next, 0);
        }
        buses.push_back(std::move(b));
        return static_cast<uint32_t>(buses.size() - 1);
    }

    void applyBusVolume(Bus& b) { ma_sound_group_set_volume(&b.group, b.volume * b.mix); }

    void setBuses(const std::vector<BusDef>& defs) {
        for (const BusDef& d : defs) {
            if (d.name.empty()) continue;
            const uint32_t index = bus(d.name);
            Bus& b = *buses[index];
            b.mix = std::max(d.volume, 0.0f);
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
                ma_node_attach_output_bus(&b.meter, 0, &buses[parent]->group, 0);
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
            const bool lowpass = d.lowpassAtMin > 0.0f || d.lowpassAtMax > 0.0f;
            ma_node* target = &buses[e.bus]->group;
            if (lowpass) {
                const ma_uint32 channels = ma_engine_get_channels(&engine);
                ma_lpf_node_config lc =
                    ma_lpf_node_config_init(channels, ma_engine_get_sample_rate(&engine), std::max(d.lowpassAtMin, 20.0f), 2);
                if (ma_lpf_node_init(ma_engine_get_node_graph(&engine), &lc, nullptr, &l.lpf) == MA_SUCCESS) {
                    l.lpf_ = true;
                    l.cutoff = std::max(d.lowpassAtMin, 20.0f);
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
        for (uint32_t i = 0; i < v.layerCount; ++i) ma_sound_start(&v.layers[i].sound);
        return true;
    }

    // Puts parameters/position/fades into the real voice
    void apply(Voice& v) {
        const EventInfo& e = events[v.event];
        const EventDef& d = e.def;
        const float t = curveT(v, d);
        float volume = d.volume * v.volumeScale * lerpf(d.volumeAtMin, d.volumeAtMax, t);
        if (v.fading)
            volume *= static_cast<float>(std::clamp((v.fadeEnd - time) / std::max(v.fadeEnd - v.fadeStart, 1e-4), 0.0, 1.0));
        const float pitch = std::max(0.01f, d.pitch * v.pitchScale * lerpf(d.pitchAtMin, d.pitchAtMax, t));
        const float cutoff = lerpf(d.lowpassAtMin, d.lowpassAtMax, t);
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
            if (l.lpf_ && cutoff > 0.0f && std::fabs(cutoff - l.cutoff) > 1.0f) {
                l.cutoff = cutoff;
                ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&engine), ma_engine_get_sample_rate(&engine),
                                                      std::max(cutoff, 20.0f), 2);
                ma_lpf_node_reinit(&lc, &l.lpf);
            }
        }
    }

    float estimate(Voice& v) const { // volume × distance attenuation (virtual voice/limit decisions)
        const EventDef& d = events[v.event].def;
        const float t = curveT(v, d);
        float gain = d.volume * v.volumeScale * lerpf(d.volumeAtMin, d.volumeAtMax, t);
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
    for (auto& c : m->clips)
        if (c->decoded) ma_sound_uninit(&c->source);
    m->clips.clear();
    for (size_t i = m->buses.size(); i-- > 0;) { // child buses first
        ma_node_uninit(&m->buses[i]->meter.base, nullptr);
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

void Mixer::update(double dt) {
    if (!m->ready) return;
    const double begin = now();
    m->time += dt;
    for (Subtitle& s : m->subtitles) s.secondsLeft -= static_cast<float>(dt);
    std::erase_if(m->subtitles, [](const Subtitle& s) { return s.secondsLeft <= 0.0f; });
    m->stats.playsThisFrame = m->stats.culledThisFrame = 0;
    m->startPending();
    m->manageVoices(dt);
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
        out.push_back({v.eventName, !v.real, m->events[v.event].def.loop, v.audibility, v.distance, m->time - v.startTime});
    }
}

const std::deque<CullInfo>& Mixer::culls() const { return m->culls; }

void Mixer::buses(std::vector<BusInfo>& out) const {
    out.clear();
    for (size_t i = 0; i < m->buses.size(); ++i) {
        const Bus& b = *m->buses[i];
        out.push_back({b.name, b.volume, b.meter.peak.load(std::memory_order_relaxed), b.mix, i ? m->buses[b.parent]->name : std::string()});
    }
}

const Stats& Mixer::stats() const { return m->stats; }

uint32_t Mixer::sampleRate() const { return m->ready ? ma_engine_get_sample_rate(&m->engine) : 0; }

uint32_t Mixer::channels() const { return m->ready ? ma_engine_get_channels(&m->engine) : 0; }

uint32_t Mixer::maxVoices() const { return m->desc.maxVoices; }

} // namespace mkaudio
