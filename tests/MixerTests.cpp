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
    CHECK(written.find("\n@format audio-events 2\n") != std::string::npos);
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

// Format 2: bus reverb/ducking, snapshots, music, occlusion keys (round trip)
void testEventFile2() {
    const char* text = "@format audio-events 2\n\nmixbus music\nduckBy voice\nduckVolume 0.3\n\nmixbus sfx\nreverb true\n\n"
                       "snapshot pause\nfade 0.5\nvolume music 0.5\nlowpass sfx 800\nlowpass music 600\nvolume x\n\n"
                       "music explore\nstems m/base.ogg m/drums.ogg\nbpm 96\nbeatsPerBar 3\ntransition beat\nstemParam tension\nbpm -1\n\n"
                       "event door\nocclusionVolume 0.2\nocclusionLowpass 500\n";
    mkaudio::EventFile f;
    std::vector<mkaudio::ParseMessage> messages;
    CHECK(mkaudio::parseEvents(text, f, &messages));
    CHECK(messages.size() == 2); // "volume x", "bpm -1"
    CHECK(f.buses.size() == 2 && f.buses[0].duckBy == "voice" && f.buses[0].duckVolume == 0.3f && f.buses[1].reverb);
    CHECK(f.snapshots.size() == 1 && f.snapshots[0].fade == 0.5f && f.snapshots[0].buses.size() == 2);
    CHECK(f.snapshots[0].buses[0].bus == "music" && f.snapshots[0].buses[0].volume == 0.5f && f.snapshots[0].buses[0].lowpass == 600.0f);
    CHECK(f.music.size() == 1 && f.music[0].stems.size() == 2 && f.music[0].bpm == 96.0f && f.music[0].beatsPerBar == 3);
    CHECK(f.music[0].transition == mkaudio::Quantize::Beat && f.music[0].stemParam == "tension");
    CHECK(f.events.size() == 1 && f.events[0].occlusionVolume == 0.2f && f.events[0].occlusionLowpass == 500.0f);
    const std::string written = mkaudio::writeEvents(f);
    CHECK(written.find("\n@format audio-events 2\n") != std::string::npos);
    mkaudio::EventFile back;
    CHECK(mkaudio::parseEvents(written, back) && mkaudio::writeEvents(back) == written);
    mkaudio::resolveClips(back, [](const std::string& name) { return "/x/" + name; });
    CHECK(back.music[0].stems[1].file == "/x/m/drums.ogg");
}

// Mixes seconds of sound with 60 fps updates and keeps every frame (left channel)
struct Recorder {
    mkaudio::Mixer& mixer;
    std::vector<float> left;
    void run(double seconds) {
        const int frames = static_cast<int>(seconds * 60.0 + 0.5);
        std::vector<float> out(800 * 2);
        for (int i = 0; i < frames; ++i) {
            mixer.update(1.0 / 60.0);
            const uint64_t got = mixer.render(out.data(), 800);
            for (uint64_t k = 0; k < got; ++k) left.push_back(out[k * 2]);
        }
    }
    float peak(size_t from, size_t to) const {
        float p = 0.0f;
        for (size_t i = from; i < to && i < left.size(); ++i) p = std::fmax(p, std::fabs(left[i]));
        return p;
    }
    float last(double seconds) const { // peak of the last seconds
        const size_t n = static_cast<size_t>(seconds * 48000.0);
        return peak(left.size() > n ? left.size() - n : 0, left.size());
    }
};

mkaudio::BusInfo busInfo(const mkaudio::Mixer& mixer, const char* name) {
    std::vector<mkaudio::BusInfo> buses;
    mixer.buses(buses);
    for (const mkaudio::BusInfo& b : buses)
        if (b.name == name) return b;
    return {};
}

void testSpatialMixMusic() {
    const std::string tone = "mkaudio_test_tone.wav", high = "mkaudio_test_high.wav", silence = "mkaudio_test_silence.wav", blip = "mkaudio_test_blip.wav";
    CHECK(writeWav(tone, 2.0, 220.0));
    CHECK(writeWav(high, 2.0, 8000.0));
    CHECK(writeWav(silence, 2.0, 0.0));
    CHECK(writeWav(blip, 0.05, 440.0));
    mkaudio::Desc desc;
    desc.noDevice = true;
    desc.seed = 3;
    mkaudio::Mixer mixer;
    CHECK(mixer.init(desc));
    Recorder rec{mixer, {}};

    std::vector<mkaudio::EventDef> events;
    events.push_back(event("hum", tone));
    events.back().loop = true;
    events.back().spatial = true;
    events.push_back(event("hiss", high));
    events.back().loop = true;
    events.push_back(event("blip", blip));
    events.back().bus = "voice";
    events.push_back(event("hum2d", tone));
    events.back().loop = true;
    events.back().bus = "music";
    mixer.setEvents(events);
    mixer.setListener({0, 0, 0}, {0, 0, 1});

    // Occlusion: quieter (and lowpassed), eased in
    mkaudio::Handle hum = mixer.play("hum", {0, 0, 1});
    rec.run(0.3);
    const float clear = rec.last(0.1);
    std::vector<mkaudio::Emitter> emitters;
    mixer.emitters(emitters);
    CHECK(emitters.size() == 1 && emitters[0].handle.index == hum.index && emitters[0].position.z == 1.0f);
    mixer.setOcclusion(hum, 1.0f);
    rec.run(0.4);
    const float blocked = rec.last(0.1);
    CHECK(clear > 0.1f && blocked < clear * 0.5f && blocked > clear * 0.1f);
    std::vector<mkaudio::VoiceInfo> voices;
    mixer.voices(voices);
    CHECK(voices.size() == 1 && voices[0].occlusion > 0.99f);
    mixer.stop(hum);

    // Reverb on a bus: a short sound leaves a tail; without reverb it's silent after the sound
    rec.run(0.2);
    mixer.play2D("blip");
    rec.run(0.3);
    CHECK(rec.last(0.1) < 0.0005f);
    mkaudio::BusDef voiceBus;
    voiceBus.name = "voice";
    voiceBus.reverb = true;
    mixer.setBuses({voiceBus});
    mixer.setReverb({0.8f, 0.3f, 0.8f, 1.0f}, 0.0f);
    rec.run(0.1);
    mixer.play2D("blip");
    rec.run(0.3);
    CHECK(rec.last(0.1) > 0.001f);
    CHECK(busInfo(mixer, "voice").reverb && mixer.reverb().wet == 0.8f);
    mixer.setReverb({}, 0.0f);
    rec.run(0.5);
    CHECK(rec.last(0.1) < 0.0005f);

    // Ducking: music goes down while voice sounds, then comes back
    mkaudio::BusDef musicBus;
    musicBus.name = "music";
    musicBus.duckBy = "voice";
    musicBus.duckVolume = 0.25f;
    musicBus.duckAttack = 0.0f;
    musicBus.duckRelease = 0.1f;
    mixer.setBuses({musicBus});
    mkaudio::Handle bed = mixer.play2D("hum2d");
    rec.run(0.2);
    CHECK(busInfo(mixer, "music").duck == 1.0f);
    mixer.play2D("blip");
    rec.run(1.0 / 30.0);
    CHECK(std::fabs(busInfo(mixer, "music").duck - 0.25f) < 0.01f);
    rec.run(0.4);
    CHECK(busInfo(mixer, "music").duck == 1.0f);
    mixer.stop(bed);

    // Snapshots: volume and lowpass, faded, several at once
    mkaudio::SnapshotDef pause;
    pause.name = "pause";
    pause.fade = 0.1f;
    pause.buses.push_back({"sfx", 1.0f, 300.0f});
    mkaudio::SnapshotDef mute;
    mute.name = "mute";
    mute.fade = 0.0f;
    mute.buses.push_back({"sfx", 0.0f, 0.0f});
    mixer.setSnapshot("pause", true); // before it's defined: kept by name
    mixer.setSnapshots({pause, mute});
    CHECK(mixer.snapshot("pause"));
    mixer.setSnapshot("pause", false);
    mkaudio::Handle hiss = mixer.play2D("hiss");
    rec.run(0.3);
    const float open = rec.last(0.1);
    mixer.setSnapshot("pause", true);
    rec.run(0.3);
    CHECK(open > 0.1f && rec.last(0.1) < open * 0.1f && busInfo(mixer, "sfx").lowpass > 0.0f);
    mixer.setSnapshot("pause", false);
    mixer.setSnapshot("mute", true);
    rec.run(0.3);
    CHECK(rec.last(0.1) < 0.001f && busInfo(mixer, "sfx").snapshot == 0.0f);
    mixer.setSnapshot("mute", false);
    rec.run(0.1);
    CHECK(busInfo(mixer, "sfx").snapshot == 1.0f && busInfo(mixer, "sfx").lowpass == 0.0f);
    mixer.stop(hiss);
    rec.run(0.1);

    // Music: the switch lands on the next bar, to the frame
    std::vector<mkaudio::MusicDef> music(3);
    music[0].name = "loud";
    music[0].stems = {{"tone", tone}};
    music[0].bpm = 600.0f; // a beat is 4800 frames, a bar 19200
    music[0].crossfade = 0.01f;
    music[1] = music[0];
    music[1].name = "quiet";
    music[1].stems = {{"silence", silence}};
    music[2] = music[0];
    music[2].name = "layers";
    music[2].stems = {{"silence", silence}, {"tone", tone}};
    music[2].stemParam = "tension";
    music[2].stemFade = 0.1f;
    mixer.setMusic(music);
    mixer.playMusic("missing");
    CHECK(culls(mixer, "missing", "missing music") == 1);
    const size_t base = rec.left.size();
    mixer.playMusic("loud");
    rec.run(0.15);
    CHECK(mixer.music().track == "loud" && mixer.music().bpm == 600.0f);
    mixer.playMusic("quiet");
    CHECK(mixer.music().track == "loud" && mixer.music().next == "quiet");
    rec.run(0.5);
    CHECK(mixer.music().track == "quiet" && mixer.music().next.empty());
    // "loud" started 50 ms (lookahead) after base, the next bar is 19200 frames later
    const size_t start = base + 2400, bar = start + 19200;
    CHECK(rec.peak(start + 2000, start + 4000) > 0.1f);
    CHECK(rec.peak(bar - 1500, bar - 1) > 0.1f);             // still the old track right before the bar
    CHECK(rec.peak(bar + 480 + 64, bar + 9000) < 0.0005f);  // gone after the crossfade
    const mkaudio::MusicInfo info = mixer.music();
    CHECK(info.beatsPerBar == 4 && info.beat >= 0.0f && info.beat < 4.0f);

    // Stinger on the next beat of the playing track ("quiet" starts at the bar)
    rec.run(0.03);
    const size_t before = rec.left.size();
    mixer.stinger("blip", mkaudio::Quantize::Beat);
    rec.run(0.3);
    size_t first = 0;
    for (size_t i = before; i < rec.left.size() && !first; ++i)
        if (std::fabs(rec.left[i]) > 0.001f) first = i;
    CHECK(first > 0 && (first - bar) % 4800 < 64);

    // Stems follow a parameter: the second stem comes in with tension
    mixer.playMusic("layers", mkaudio::Quantize::Immediate);
    rec.run(0.3);
    CHECK(mixer.music().track == "layers" && rec.last(0.1) < 0.0005f);
    mixer.setGlobalParam("tension", 1.0f);
    rec.run(0.3);
    CHECK(rec.last(0.1) > 0.1f);
    mixer.stopMusic(0.05f);
    rec.run(0.3);
    CHECK(mixer.music().track.empty() && rec.last(0.1) < 0.0005f);

    mixer.shutdown();
    for (const std::string& f : {tone, high, silence, blip}) std::remove(f.c_str());
}

} // namespace

int main() {
    testEventFile();
    testEventFile2();
    testSpatialMixMusic();
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
