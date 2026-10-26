#include "mkaudio/Events.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mkaudio {
namespace {

// 이벤트 키 하나: 파일 이름과 EventDef 필드
enum class Kind : uint8_t { Float, Bool, UInt, String, Clips, ContainerKind, StealKind };
struct Key {
    const char* name;
    Kind kind;
    float EventDef::*f = nullptr;
    bool EventDef::*b = nullptr;
    uint32_t EventDef::*u = nullptr;
    std::string EventDef::*s = nullptr;
};

// 쓰는 순서 = 이 표의 순서
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

// 키 하나를 읽는다. 값이 틀리면 false
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

// 기본값과 다르면 "키 값" 줄
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

// UTF-8 경로 (C++20: u8path 대신 char8_t 문자열)
std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

} // namespace

const char* containerName(Container c) { return c == Container::Blend ? "blend" : c == Container::Switch ? "switch" : "random"; }
const char* stealName(Steal s) { return s == Steal::Quietest ? "quietest" : s == Steal::None ? "none" : "oldest"; }

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
    enum class Block { None, Event, Bus, Skip } block = Block::None;
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
        if (key == "event" || key == "mixbus") { // 블록 머리 ("bus"는 이벤트 안의 키라 버스 블록은 mixbus)
            bool duplicate = false;
            for (const EventDef& e : out.events) duplicate = duplicate || (key == "event" && e.name == value);
            for (const BusDef& b : out.buses) duplicate = duplicate || (key == "mixbus" && b.name == value);
            if (!validName(value) || duplicate) {
                note(lineNo, true, std::string(duplicate ? "duplicate " : "invalid ") + key + " name '" + value + "' skipped");
                block = Block::Skip;
                continue;
            }
            if (key == "event") {
                out.events.emplace_back();
                out.events.back().name = value;
                block = Block::Event;
            } else {
                out.buses.emplace_back();
                out.buses.back().name = value;
                block = Block::Bus;
            }
            continue;
        }
        if (block == Block::Skip) continue;
        if (block == Block::None) {
            note(lineNo, true, "'" + key + "' before the first event or mixbus");
            continue;
        }
        if (block == Block::Bus) {
            BusDef& b = out.buses.back();
            if (key == "parent") b.parent = value;
            else if (key == "volume") {
                if (!parseFloat(value, b.volume)) note(lineNo, true, "bad value for volume: '" + value + "'");
            } else note(lineNo, false, "unknown bus key '" + key + "' ignored");
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
    std::string out = "# MealKit audio events. Edit them in MealKit Audio or by hand: \"event <name>\" or \"mixbus <name>\", then \"key value\" lines.\n";
    out += std::string("@format ") + kEventsFormatKind + " " + std::to_string(kEventsFormatVersion) + "\n";
    static const BusDef defaultBus;
    for (const BusDef& b : file.buses) {
        out += "\nmixbus " + b.name + "\n";
        if (b.parent != defaultBus.parent) out += "parent " + b.parent + "\n";
        if (b.volume != defaultBus.volume) out += "volume " + number(b.volume) + "\n";
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
    std::filesystem::rename(temp, target, ec); // 같은 폴더라 바꿔 넣기
    if (ec) std::filesystem::remove(temp, ec);
    return !ec;
}

void resolveClips(EventFile& file, const std::function<std::string(const std::string&)>& resolve) {
    for (EventDef& e : file.events)
        for (ClipRef& c : e.clips) c.file = resolve(c.name);
}

} // namespace mkaudio
