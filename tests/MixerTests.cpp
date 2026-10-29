// mkaudio tests: mixes without a device (Desc::noDevice) and pulls output with render(). Clips are WAV files made by the test.
#include "mkaudio/Events.h"
#include "mkaudio/Mixer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failed = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failed;                                                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                               \
        }                                                                                                              \
    } while (0)

// 16-bit mono sine wave WAV
bool writeWav(const std::string& path, double seconds, double hz, unsigned rate = 48000) {
    const unsigned frames = static_cast<unsigned>(seconds * rate);
    std::vector<short> samples(frames);
    for (unsigned i = 0; i < frames; ++i) samples[i] = static_cast<short>(std::sin(i * hz * 6.283185307179586 / rate) * 16000.0);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const unsigned data = frames * 2, riff = 36 + data, fmtSize = 16, byteRate = rate * 2;
    const unsigned short pcm = 1, channels = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtSize, 4, 1, f);
    std::fwrite(&pcm, 2, 1, f);
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&data, 4, 1, f);
    std::fwrite(samples.data(), 2, frames, f);
    std::fclose(f);
    return true;
}

// One frame (60 fps): update, then mix that much time and return the peak
float frame(mkaudio::Mixer& mixer) {
    mixer.update(1.0 / 60.0);
    std::vector<float> out(800 * mixer.channels());
    const uint64_t got = mixer.render(out.data(), 800);
    float peak = 0.0f;
    for (uint64_t i = 0; i < got * mixer.channels(); ++i) peak = std::fmax(peak, std::fabs(out[i]));
    return peak;
}

size_t culls(const mkaudio::Mixer& mixer, const char* event, const char* reason) {
    size_t n = 0;
    for (const mkaudio::CullInfo& c : mixer.culls())
        if (c.event == event && std::strcmp(c.reason, reason) == 0) ++n;
    return n;
}

mkaudio::EventDef event(const char* name, const std::string& file) {
    mkaudio::EventDef e;
    e.name = name;
    e.clips.push_back({name, file});
    e.spatial = false;
    return e;
}

// Event files: read/write round trip, defaults aren't written, bad lines become messages
void testEventFile() {
    const char* text = "# comment\n@format audio-events 1\n\nmixbus music\nvolume 0.5\n\nmixbus ambience\nparent music\n\n"
                       "event hit\nclips a.wav  b.wav\ncontainer blend\nparam speed\nparamMax 15\nvolumeRandom 0.15\nspatial false\nbus ambience\n"
                       "steal quietest\nmaxInstances 3\nsubtitle sub.hit\nfuture 1\n\nevent hit\nvolume 2\n\nevent bad name\n\nevent quiet\nvolume x\n";
    mkaudio::EventFile f;
    std::vector<mkaudio::ParseMessage> messages;
    CHECK(mkaudio::parseEvents(text, f, &messages));
    CHECK(f.buses.size() == 2 && f.buses[0].name == "music" && f.buses[0].volume == 0.5f && f.buses[1].parent == "music");
    CHECK(f.events.size() == 2 && f.events[0].name == "hit" && f.events[1].name == "quiet");
    const mkaudio::EventDef& hit = f.events[0];
    CHECK(hit.clips.size() == 2 && hit.clips[1].name == "b.wav" && hit.clips[1].file.empty());
    CHECK(hit.container == mkaudio::Container::Blend && hit.param == "speed" && hit.paramMax == 15.0f && !hit.spatial);
    CHECK(hit.steal == mkaudio::Steal::Quietest && hit.maxInstances == 3 && hit.subtitle == "sub.hit" && hit.bus == "ambience");
    size_t errors = 0, warnings = 0;
    for (const auto& m : messages) (m.error ? errors : warnings)++;
    CHECK(errors == 3 && warnings == 1); // duplicate name, bad name, bad value / unknown key

    const std::string written = mkaudio::writeEvents(f);
    CHECK(written.find("\n@format audio-events 1\n") != std::string::npos);
    CHECK(written.find("pitch ") == std::string::npos); // defaults aren't written
    CHECK(written.find("volumeRandom 0.15\n") != std::string::npos && written.find("spatial false\n") != std::string::npos);
    mkaudio::EventFile back;
    CHECK(mkaudio::parseEvents(written, back) && mkaudio::writeEvents(back) == written);

    CHECK(!mkaudio::parseEvents("@format table 1\nrow x\n", back)); // a file of another kind
    CHECK(mkaudio::saveEventFile("mkaudio_test.events", f) && mkaudio::loadEventFile("mkaudio_test.events", back) && back.events.size() == 2);
    std::remove("mkaudio_test.events");
    mkaudio::resolveClips(back, [](const std::string& name) { return name == "a.wav" ? std::string("/x/a.wav") : std::string(); });
    CHECK(back.events[0].clips[0].file == "/x/a.wav" && back.events[0].clips[1].file.empty());
}

} // namespace

int main() {
    testEventFile();
    const std::string beepFile = "mkaudio_test_beep.wav", humFile = "mkaudio_test_hum.wav";
    CHECK(writeWav(beepFile, 0.3, 440.0));
    CHECK(writeWav(humFile, 0.5, 220.0));

    std::vector<std::string> log;
    mkaudio::Desc desc;
    desc.noDevice = true;
    desc.seed = 7;
    desc.maxVoices = 4;
    desc.log = [&](mkaudio::LogLevel, const std::string& text) { log.push_back(text); };
    mkaudio::Mixer mixer;
    CHECK(mixer.init(desc));
    CHECK(mixer.ready() && mixer.sampleRate() == 48000 && mixer.channels() == 2 && mixer.maxVoices() == 4);

    std::vector<mkaudio::EventDef> events;
    events.push_back(event("beep", beepFile));
    mkaudio::EventDef cool = event("cool", beepFile);
    cool.cooldown = 10.0f;
    events.push_back(cool);
    mkaudio::EventDef cap = event("cap", beepFile);
    cap.maxInstances = 1;
    cap.steal = mkaudio::Steal::None;
    events.push_back(cap);
    mkaudio::EventDef hum = event("hum", humFile);
    hum.loop = true;
    hum.spatial = true;
    hum.minDistance = 1.0f;
    hum.maxDistance = 10.0f;
    events.push_back(hum);
    events.push_back(event("missing", ""));
    mkaudio::EventDef sub = event("sub", beepFile);
    sub.subtitle = "subtitle.beep";
    events.push_back(sub);
    mixer.setEvents(events);
    CHECK(mixer.eventNames().size() == 6 && mixer.event("cool") && mixer.event("cool")->cooldown == 10.0f);

    // Playing is audible, stopping invalidates it
    mkaudio::Handle h = mixer.play2D("beep");
    CHECK(h.valid() && mixer.playing(h));
    CHECK(frame(mixer) > 0.05f && mixer.stats().realVoices == 1 && mixer.stats().playsThisFrame == 1);
    mixer.stop(h);
    CHECK(!mixer.playing(h));
    frame(mixer); // one chunk left in the node graph
    CHECK(frame(mixer) < 0.01f);

    // Missing events, missing clips
    CHECK(!mixer.play2D("nope").valid() && culls(mixer, "nope", "missing event") == 1);
    mixer.play2D("missing");
    frame(mixer);
    CHECK(culls(mixer, "missing", "missing clip") == 1);

    // Cooldown
    mixer.play2D("cool");
    frame(mixer);
    mixer.play2D("cool");
    frame(mixer);
    CHECK(culls(mixer, "cool", "cooldown") == 1);

    // Same-frame grouping, concurrency limit (no cutting)
    for (int i = 0; i < 3; ++i) mixer.play2D("cap");
    frame(mixer);
    CHECK(culls(mixer, "cap", "same frame") == 2);
    mixer.play2D("cap");
    frame(mixer);
    CHECK(culls(mixer, "cap", "max instances") == 1);
    mixer.stopAll();

    // Distant loops become virtual voices, and are mixed again when approached
    mixer.setListener({0, 0, 0}, {0, 0, 1});
    mkaudio::Handle loop = mixer.play("hum", {0, 0, 50});
    frame(mixer);
    CHECK(mixer.playing(loop) && mixer.stats().virtualVoices == 1 && mixer.stats().realVoices == 0);
    mixer.setPosition(loop, {0, 0, 2});
    CHECK(frame(mixer) > 0.01f && mixer.stats().realVoices == 1);
    std::vector<mkaudio::VoiceInfo> voices;
    mixer.voices(voices);
    CHECK(voices.size() == 1 && voices[0].event == "hum" && voices[0].loop && !voices[0].virtualVoice);
    mixer.stop(loop, 0.1f); // fade
    for (int i = 0; i < 10; ++i) frame(mixer);
    CHECK(!mixer.playing(loop));

    // Subtitles: shown regardless of whether the sound is audible
    mixer.setMuted(true);
    mixer.play2D("sub");
    CHECK(frame(mixer) == 0.0f);
    CHECK(mixer.subtitles().size() == 1 && mixer.subtitles()[0].key == "subtitle.beep");
    mixer.setMuted(false);

    // Buses
    std::vector<mkaudio::BusInfo> buses;
    mixer.buses(buses);
    CHECK(buses.size() == 5 && buses[0].name == "master" && buses[2].name == "sfx");
    mixer.setBusVolume("sfx", 0.0f);
    CHECK(mixer.busVolume("sfx") == 0.0f);
    mixer.play2D("beep");
    frame(mixer); // volume changes take effect from the next chunk
    CHECK(frame(mixer) < 0.001f);
    // Bus definitions: parent/mix volume, cyclic parents go to master
    mixer.setBuses({{"ambience", "music", 0.5f}, {"music", "ambience", 1.0f}});
    mixer.buses(buses);
    const auto busNamed = [&](const char* name) {
        for (const mkaudio::BusInfo& b : buses)
            if (b.name == name) return b;
        return mkaudio::BusInfo{};
    };
    CHECK(busNamed("ambience").parent == "music" && busNamed("ambience").mix == 0.5f);
    CHECK(busNamed("music").parent == "master"); // music → ambience → music cycle

    // Changing definitions keeps playing sounds by name, and vanished events stop
    mixer.setBusVolume("sfx", 1.0f);
    mkaudio::Handle keep = mixer.play2D("beep"), gone = mixer.play2D("sub");
    frame(mixer);
    events.erase(events.end() - 1);
    mixer.setEvents(events);
    CHECK(mixer.playing(keep) && !mixer.playing(gone));

    mixer.shutdown();
    CHECK(!mixer.ready());
    std::remove(beepFile.c_str());
    std::remove(humFile.c_str());

    std::printf("%s: %d check(s), %d failed\n", g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
