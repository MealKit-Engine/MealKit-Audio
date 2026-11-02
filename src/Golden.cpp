#include "mkaudio/Golden.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace mkaudio {
namespace {

float toDb(double value) { return value <= 0.0 ? kGoldenFloorDb : std::max(kGoldenFloorDb, static_cast<float>(20.0 * std::log10(value))); }

// One decimal is enough for dB and keeps the files stable
float round1(float v) { return std::round(v * 10.0f) / 10.0f; }

void writeList(std::string& out, const char* key, const std::vector<float>& values) {
    out += key;
    char buffer[16];
    for (float v : values) {
        std::snprintf(buffer, sizeof(buffer), " %.1f", static_cast<double>(v));
        out += buffer;
    }
    out += "\n";
}

} // namespace

bool renderEnvelope(const EventFile& file, const std::string& event, Envelope& out, std::string& error, const GoldenOptions& options) {
    out = {};
    out.event = event;
    const auto def = std::find_if(file.events.begin(), file.events.end(), [&](const EventDef& e) { return e.name == event; });
    if (def == file.events.end()) {
        error = "no event named " + event;
        return false;
    }
    Desc desc;
    desc.noDevice = true;
    desc.sampleRate = out.sampleRate;
    desc.seed = options.seed;
    Mixer mixer;
    if (!mixer.init(desc)) {
        error = "cannot start the mixer";
        return false;
    }
    mixer.setBuses(file.buses);
    mixer.setEvents(file.events);
    const Handle handle = mixer.play2D(event);

    const uint32_t channels = mixer.channels();
    const uint32_t frameFrames = out.sampleRate / 60; // update at 60 fps, like a game
    std::vector<float> buffer(static_cast<size_t>(frameFrames) * channels);
    double sumSquares = 0.0, peak = 0.0;
    uint32_t inBlock = 0;
    const double limit = def->loop ? std::min(options.loopSeconds, options.maxSeconds) : options.maxSeconds;
    double time = 0.0, endedAt = -1.0;
    bool started = false;
    while (time < limit + options.tailSeconds) {
        if (endedAt < 0.0 && time >= limit) {
            mixer.stop(handle);
            endedAt = time;
        }
        mixer.update(1.0 / 60.0);
        const bool playing = mixer.playing(handle);
        started = started || playing;
        if (started && !playing && endedAt < 0.0) endedAt = time;
        if (endedAt >= 0.0 && time >= endedAt + options.tailSeconds) break;
        const uint64_t got = mixer.render(buffer.data(), frameFrames);
        for (uint64_t f = 0; f < got; ++f) {
            for (uint32_t c = 0; c < channels; ++c) {
                const double s = buffer[f * channels + c];
                sumSquares += s * s;
                peak = std::max(peak, std::fabs(s));
            }
            if (++inBlock == out.blockFrames) {
                out.rms.push_back(round1(toDb(std::sqrt(sumSquares / (static_cast<double>(inBlock) * channels)))));
                out.peak.push_back(round1(toDb(peak)));
                sumSquares = peak = 0.0;
                inBlock = 0;
            }
        }
        time += static_cast<double>(frameFrames) / out.sampleRate;
    }
    if (!started) {
        error = "the event didn't play (missing clip, or culled)";
        return false;
    }
    // Trailing silence is not part of the sound
    while (!out.rms.empty() && out.rms.back() <= kGoldenFloorDb && out.peak.back() <= kGoldenFloorDb) {
        out.rms.pop_back();
        out.peak.pop_back();
    }
    return true;
}

std::string writeEnvelope(const Envelope& envelope) {
    std::string out = "# Golden render of a sound event (mkaudio/Golden.h). Rewrite with MealKitAudio --golden --update\n";
    out += "@format audio-golden 1\n";
    out += "event " + envelope.event + "\n";
    out += "rate " + std::to_string(envelope.sampleRate) + "\n";
    out += "block " + std::to_string(envelope.blockFrames) + "\n";
    writeList(out, "rms", envelope.rms);
    writeList(out, "peak", envelope.peak);
    return out;
}

bool parseEnvelope(const std::string& text, Envelope& out, std::string& error) {
    out = {};
    std::istringstream in(text);
    std::string line;
    bool format = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream words(line);
        std::string key;
        words >> key;
        if (key == "@format") {
            std::string kind;
            uint32_t version = 0;
            words >> kind >> version;
            if (kind != "audio-golden" || version != 1) {
                error = "not an audio-golden 1 file";
                return false;
            }
            format = true;
        } else if (key == "event") words >> out.event;
        else if (key == "rate") words >> out.sampleRate;
        else if (key == "block") words >> out.blockFrames;
        else if (key == "rms" || key == "peak") {
            std::vector<float>& list = key == "rms" ? out.rms : out.peak;
            for (float v; words >> v;) list.push_back(v);
        }
    }
    if (!format) {
        error = "missing @format line";
        return false;
    }
    if (out.rms.size() != out.peak.size() || out.blockFrames == 0 || out.sampleRate == 0) {
        error = "rms and peak differ in length, or no rate/block";
        return false;
    }
    return true;
}

GoldenDiff compareEnvelopes(const Envelope& expected, const Envelope& actual, float toleranceDb, float floorDb, uint32_t lengthTolerance) {
    GoldenDiff diff;
    char buffer[200];
    if (expected.sampleRate != actual.sampleRate || expected.blockFrames != actual.blockFrames) {
        diff.same = false;
        diff.reason = "rate or block size differs";
        return diff;
    }
    const double blockSeconds = static_cast<double>(expected.blockFrames) / expected.sampleRate;
    const size_t a = expected.rms.size(), b = actual.rms.size();
    if ((a > b ? a - b : b - a) > lengthTolerance) {
        std::snprintf(buffer, sizeof(buffer), "length %.2f s, expected %.2f s", b * blockSeconds, a * blockSeconds);
        diff.same = false;
        diff.reason = buffer;
        return diff;
    }
    const auto at = [&](const std::vector<float>& list, size_t i) { return std::max(floorDb, i < list.size() ? list[i] : kGoldenFloorDb); };
    for (size_t i = 0; i < std::max(a, b); ++i) {
        for (int k = 0; k < 2; ++k) {
            const std::vector<float>& e = k == 0 ? expected.rms : expected.peak;
            const std::vector<float>& g = k == 0 ? actual.rms : actual.peak;
            const float x = at(e, i), y = at(g, i);
            if (std::fabs(x - y) <= toleranceDb) continue;
            std::snprintf(buffer, sizeof(buffer), "%s at %.2f s: %.1f dB, expected %.1f dB", k == 0 ? "rms" : "peak", i * blockSeconds,
                          static_cast<double>(y), static_cast<double>(x));
            diff.same = false;
            diff.reason = buffer;
            return diff;
        }
    }
    return diff;
}

} // namespace mkaudio
