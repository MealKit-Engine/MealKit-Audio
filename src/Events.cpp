#include "mkaudio/Events.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mkaudio {
namespace {

// One event key: file name and EventDef field
enum class Kind : uint8_t { Float, Bool, UInt, String, Clips, ContainerKind, StealKind };
struct Key {
    const char* name;
    Kind kind;
    float EventDef::*f = nullptr;
    bool EventDef::*b = nullptr;
    uint32_t EventDef::*u = nullptr;
    std::string EventDef::*s = nullptr;
};

// Write order = the order of this table
const Key kKeys[] = {
    {"clips", Kind::Clips},
    {"container", Kind::ContainerKind},
    {"param", Kind::String, nullptr, nullptr, nullptr, &EventDef::param},
    {"paramMin", Kind::Float, &EventDef::paramMin},
    {"paramMax", Kind::Float, &EventDef::paramMax},
    {"volume", Kind::Float, &EventDef::volume},
    {"volumeRandom", Kind::Float, &EventDef::volumeRandom},
    {"pitch", Kind::Float, &EventDef::pitch},
    {"pitchRandom", Kind::Float, &EventDef::pitchRandom},
    {"volumeAtMin", Kind::Float, &EventDef::volumeAtMin},
    {"volumeAtMax", Kind::Float, &EventDef::volumeAtMax},
    {"pitchAtMin", Kind::Float, &EventDef::pitchAtMin},
    {"pitchAtMax", Kind::Float, &EventDef::pitchAtMax},
    {"lowpassAtMin", Kind::Float, &EventDef::lowpassAtMin},
    {"lowpassAtMax", Kind::Float, &EventDef::lowpassAtMax},
    {"spatial", Kind::Bool, nullptr, &EventDef::spatial},
    {"minDistance", Kind::Float, &EventDef::minDistance},
    {"maxDistance", Kind::Float, &EventDef::maxDistance},
    {"bus", Kind::String, nullptr, nullptr, nullptr, &EventDef::bus},
    {"loop", Kind::Bool, nullptr, &EventDef::loop},
    {"stream", Kind::Bool, nullptr, &EventDef::stream},
    {"priority", Kind::UInt, nullptr, nullptr, &EventDef::priority},
    {"maxInstances", Kind::UInt, nullptr, nullptr, &EventDef::maxInstances},
    {"steal", Kind::StealKind},
    {"cooldown", Kind::Float, &EventDef::cooldown},
    {"subtitle", Kind::String, nullptr, nullptr, nullptr, &EventDef::subtitle},
    {"subtitleSeconds", Kind::Float, &EventDef::subtitleSeconds},
    {"occlusionVolume", Kind::Float, &EventDef::occlusionVolume},
    {"occlusionLowpass", Kind::Float, &EventDef::occlusionLowpass},
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
}

bool parseFloat(const std::string& text, float& out) {
    char* end = nullptr;
    const float v = std::strtof(text.c_str(), &end);
    if (text.empty() || !end || *end) return false;
    out = v;
    return true;
}

bool parseUInt(const std::string& text, uint32_t& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(text.c_str(), &end, 10);
    if (text.empty() || !end || *end || text[0] == '-') return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool parseBool(const std::string& text, bool& out) {
    if (text == "true" || text == "1") out = true;
    else if (text == "false" || text == "0") out = false;
    else return false;
    return true;
}

std::vector<ClipRef> splitClips(const std::string& text) {
    std::vector<ClipRef> out;
    std::istringstream in(text);
    std::string name;
    while (in >> name) out.push_back({name, {}});
    return out;
}

// Reads one key. false if the value is bad
bool setKey(EventDef& e, const Key& k, const std::string& value) {
    switch (k.kind) {
    case Kind::Float: return parseFloat(value, e.*k.f);
    case Kind::Bool: return parseBool(value, e.*k.b);
    case Kind::UInt: return parseUInt(value, e.*k.u);
    case Kind::String: e.*k.s = value; return true;
    case Kind::Clips: e.clips = splitClips(value); return true;
    case Kind::ContainerKind:
        if (value == "random") e.container = Container::Random;
        else if (value == "switch") e.container = Container::Switch;
        else if (value == "blend") e.container = Container::Blend;
        else return false;
        return true;
    case Kind::StealKind:
        if (value == "oldest") e.steal = Steal::Oldest;
        else if (value == "quietest") e.steal = Steal::Quietest;
        else if (value == "none") e.steal = Steal::None;
        else return false;
        return true;
    }
    return false;
}

// A "key value" line if different from the default
std::string keyLine(const EventDef& e, const Key& k) {
    static const EventDef d;
    std::string value;
    switch (k.kind) {
    case Kind::Float:
        if (e.*k.f == d.*k.f) return {};
        value = number(e.*k.f);
        break;
    case Kind::Bool:
        if (e.*k.b == d.*k.b) return {};
        value = e.*k.b ? "true" : "false";
        break;
    case Kind::UInt:
        if (e.*k.u == d.*k.u) return {};
        value = std::to_string(e.*k.u);
        break;
    case Kind::String:
        if (e.*k.s == d.*k.s) return {};
        value = e.*k.s;
        break;
    case Kind::Clips:
        for (const ClipRef& c : e.clips) value += (value.empty() ? "" : " ") + c.name;
        if (value.empty()) return {};
        break;
    case Kind::ContainerKind:
        if (e.container == d.container) return {};
        value = containerName(e.container);
        break;
    case Kind::StealKind:
        if (e.steal == d.steal) return {};
        value = stealName(e.steal);
        break;
    }
    return std::string(k.name) + " " + value + "\n";
}

bool parseQuantize(const std::string& text, Quantize& out) {
    if (text == "immediate") out = Quantize::Immediate;
    else if (text == "beat") out = Quantize::Beat;
    else if (text == "bar") out = Quantize::Bar;
    else return false;
    return true;
}

// Keys of the other blocks: -1 unknown, 0 bad value, 1 read
int setBusKey(BusDef& b, const std::string& key, const std::string& value) {
    if (key == "parent") b.parent = value;
    else if (key == "duckBy") b.duckBy = value;
    else if (key == "volume") return parseFloat(value, b.volume);
    else if (key == "reverb") return parseBool(value, b.reverb);
    else if (key == "duckVolume") return parseFloat(value, b.duckVolume);
    else if (key == "duckAttack") return parseFloat(value, b.duckAttack);
    else if (key == "duckRelease") return parseFloat(value, b.duckRelease);
    else return -1;
    return 1;
}

int setSnapshotKey(SnapshotDef& s, const std::string& key, const std::string& value) {
    if (key == "fade") return parseFloat(value, s.fade);
    if (key != "volume" && key != "lowpass") return -1;
    const size_t sp = value.find(' '); // "<bus> <value>"
    if (sp == std::string::npos) return 0;
    const std::string bus = value.substr(0, sp);
    float v = 0.0f;
    if (!validName(bus) || !parseFloat(std::string(trim(std::string_view(value).substr(sp + 1))), v)) return 0;
    SnapshotBus* sb = nullptr;
    for (SnapshotBus& existing : s.buses)
        if (existing.bus == bus) sb = &existing;
    if (!sb) {
        s.buses.emplace_back();
        sb = &s.buses.back();
        sb->bus = bus;
    }
    (key == "volume" ? sb->volume : sb->lowpass) = v;
    return 1;
}

int setMusicKey(MusicDef& m, const std::string& key, const std::string& value) {
    if (key == "stems") m.stems = splitClips(value);
    else if (key == "stemParam") m.stemParam = value;
    else if (key == "bus") m.bus = value;
    else if (key == "bpm") {
        float v = 0.0f;
        if (!parseFloat(value, v) || v <= 0.0f) return 0;
        m.bpm = v;
    } else if (key == "beatsPerBar") {
        uint32_t v = 0;
        if (!parseUInt(value, v) || v == 0) return 0;
        m.beatsPerBar = v;
    }
    else if (key == "offset") return parseFloat(value, m.offset);
    else if (key == "transition") return parseQuantize(value, m.transition);
    else if (key == "crossfade") return parseFloat(value, m.crossfade);
    else if (key == "paramMin") return parseFloat(value, m.paramMin);
    else if (key == "paramMax") return parseFloat(value, m.paramMax);
    else if (key == "stemFade") return parseFloat(value, m.stemFade);
    else if (key == "volume") return parseFloat(value, m.volume);
    else if (key == "loop") return parseBool(value, m.loop);
    else return -1;
    return 1;
}

// UTF-8 path (C++20: a char8_t string instead of u8path)
std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

} // namespace

const char* containerName(Container c) { return c == Container::Blend ? "blend" : c == Container::Switch ? "switch" : "random"; }
const char* stealName(Steal s) { return s == Steal::Quietest ? "quietest" : s == Steal::None ? "none" : "oldest"; }
const char* quantizeName(Quantize q) { return q == Quantize::Immediate ? "immediate" : q == Quantize::Beat ? "beat" : "bar"; }

bool validName(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

bool parseEvents(std::string_view text, EventFile& out, std::vector<ParseMessage>* messages) {
    out = {};
    auto note = [&](uint32_t line, bool error, std::string msg) {
        if (messages) messages->push_back({line, error, std::move(msg)});
    };
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3); // BOM
    enum class Block { None, Event, Bus, Snapshot, Music, Skip } block = Block::None;
    uint32_t lineNo = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        const size_t space = line.find_first_of(" \t");
        const std::string key(line.substr(0, space));
        const std::string value(space == std::string_view::npos ? std::string_view() : trim(line.substr(space + 1)));

        if (key == "@format") { // "@format audio-events <N>"
            const size_t sp = value.find(' ');
            const std::string kind = value.substr(0, sp);
            uint32_t version = 0;
            if (kind != kEventsFormatKind) {
                note(lineNo, true, "this is a '" + kind + "' file, not " + kEventsFormatKind);
                return false;
            }
            if (sp == std::string::npos || !parseUInt(value.substr(sp + 1), version) || version == 0) note(lineNo, false, "bad format line");
            else if (version > kEventsFormatVersion)
                note(lineNo, false, "written in format " + std::to_string(version) + ", this reads up to " + std::to_string(kEventsFormatVersion) +
                                        " - unknown keys are ignored");
            continue;
        }
        if (key == "event" || key == "mixbus" || key == "snapshot" || key == "music") { // block header ("bus" is a key inside events, so bus blocks are mixbus)
            bool duplicate = false;
            for (const EventDef& e : out.events) duplicate = duplicate || (key == "event" && e.name == value);
            for (const BusDef& b : out.buses) duplicate = duplicate || (key == "mixbus" && b.name == value);
            for (const SnapshotDef& sn : out.snapshots) duplicate = duplicate || (key == "snapshot" && sn.name == value);
            for (const MusicDef& mu : out.music) duplicate = duplicate || (key == "music" && mu.name == value);
            if (!validName(value) || duplicate) {
                note(lineNo, true, std::string(duplicate ? "duplicate " : "invalid ") + key + " name '" + value + "' skipped");
                block = Block::Skip;
                continue;
            }
            if (key == "event") {
                out.events.emplace_back();
                out.events.back().name = value;
                block = Block::Event;
            } else if (key == "mixbus") {
                out.buses.emplace_back();
                out.buses.back().name = value;
                block = Block::Bus;
            } else if (key == "snapshot") {
                out.snapshots.emplace_back();
                out.snapshots.back().name = value;
                block = Block::Snapshot;
            } else {
                out.music.emplace_back();
                out.music.back().name = value;
                block = Block::Music;
            }
            continue;
        }
        if (block == Block::Skip) continue;
        if (block == Block::None) {
            note(lineNo, true, "'" + key + "' before the first event, mixbus, snapshot or music");
            continue;
        }
        if (block != Block::Event) {
            const int r = block == Block::Bus        ? setBusKey(out.buses.back(), key, value)
                          : block == Block::Snapshot ? setSnapshotKey(out.snapshots.back(), key, value)
                                                     : setMusicKey(out.music.back(), key, value);
            const char* what = block == Block::Bus ? "bus" : block == Block::Snapshot ? "snapshot" : "music";
            if (r < 0) note(lineNo, false, std::string("unknown ") + what + " key '" + key + "' ignored");
            else if (r == 0) note(lineNo, true, "bad value for " + key + ": '" + value + "'");
            continue;
        }
        const Key* k = nullptr;
        for (const Key& candidate : kKeys)
            if (key == candidate.name) k = &candidate;
        if (!k) note(lineNo, false, "unknown key '" + key + "' ignored");
        else if (!setKey(out.events.back(), *k, value)) note(lineNo, true, "bad value for " + key + ": '" + value + "'");
    }
    return true;
}

std::string writeEvents(const EventFile& file) {
    std::string out = "# MealKit audio events. Edit them in MealKit Audio or by hand: \"event|mixbus|snapshot|music <name>\", then \"key value\" lines.\n";
    out += std::string("@format ") + kEventsFormatKind + " " + std::to_string(kEventsFormatVersion) + "\n";
    static const BusDef defaultBus;
    for (const BusDef& b : file.buses) {
        out += "\nmixbus " + b.name + "\n";
        if (b.parent != defaultBus.parent) out += "parent " + b.parent + "\n";
        if (b.volume != defaultBus.volume) out += "volume " + number(b.volume) + "\n";
        if (b.reverb != defaultBus.reverb) out += std::string("reverb ") + (b.reverb ? "true" : "false") + "\n";
        if (b.duckBy != defaultBus.duckBy) out += "duckBy " + b.duckBy + "\n";
        if (b.duckVolume != defaultBus.duckVolume) out += "duckVolume " + number(b.duckVolume) + "\n";
        if (b.duckAttack != defaultBus.duckAttack) out += "duckAttack " + number(b.duckAttack) + "\n";
        if (b.duckRelease != defaultBus.duckRelease) out += "duckRelease " + number(b.duckRelease) + "\n";
    }
    static const SnapshotDef defaultSnapshot;
    for (const SnapshotDef& sn : file.snapshots) {
        out += "\nsnapshot " + sn.name + "\n";
        if (sn.fade != defaultSnapshot.fade) out += "fade " + number(sn.fade) + "\n";
        for (const SnapshotBus& sb : sn.buses) {
            if (sb.volume != 1.0f) out += "volume " + sb.bus + " " + number(sb.volume) + "\n";
            if (sb.lowpass != 0.0f) out += "lowpass " + sb.bus + " " + number(sb.lowpass) + "\n";
        }
    }
    static const MusicDef d;
    for (const MusicDef& mu : file.music) {
        out += "\nmusic " + mu.name + "\n";
        std::string stems;
        for (const ClipRef& c : mu.stems) stems += (stems.empty() ? "" : " ") + c.name;
        if (!stems.empty()) out += "stems " + stems + "\n";
        const auto num = [&](const char* key, float v, float def) {
            if (v != def) out += std::string(key) + " " + number(v) + "\n";
        };
        num("bpm", mu.bpm, d.bpm);
        if (mu.beatsPerBar != d.beatsPerBar) out += "beatsPerBar " + std::to_string(mu.beatsPerBar) + "\n";
        num("offset", mu.offset, d.offset);
        if (mu.transition != d.transition) out += std::string("transition ") + quantizeName(mu.transition) + "\n";
        num("crossfade", mu.crossfade, d.crossfade);
        if (mu.stemParam != d.stemParam) out += "stemParam " + mu.stemParam + "\n";
        num("paramMin", mu.paramMin, d.paramMin);
        num("paramMax", mu.paramMax, d.paramMax);
        num("stemFade", mu.stemFade, d.stemFade);
        num("volume", mu.volume, d.volume);
        if (mu.bus != d.bus) out += "bus " + mu.bus + "\n";
        if (mu.loop != d.loop) out += std::string("loop ") + (mu.loop ? "true" : "false") + "\n";
    }
    for (const EventDef& e : file.events) {
        out += "\nevent " + e.name + "\n";
        for (const Key& k : kKeys) out += keyLine(e, k);
    }
    return out;
}

bool loadEventFile(const std::string& path, EventFile& out, std::vector<ParseMessage>* messages) {
    std::ifstream in(pathOf(path), std::ios::binary);
    if (!in) {
        out = {};
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return parseEvents(text.str(), out, messages);
}

bool saveEventFile(const std::string& path, const EventFile& file) {
    const std::filesystem::path target = pathOf(path);
    std::filesystem::path temp = target;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::string text = writeEvents(file);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec); // same folder, so it's a swap
    if (ec) std::filesystem::remove(temp, ec);
    return !ec;
}

void resolveClips(EventFile& file, const std::function<std::string(const std::string&)>& resolve) {
    for (EventDef& e : file.events)
        for (ClipRef& c : e.clips) c.file = resolve(c.name);
    for (MusicDef& m : file.music)
        for (ClipRef& c : m.stems) c.file = resolve(c.name);
}

} // namespace mkaudio
