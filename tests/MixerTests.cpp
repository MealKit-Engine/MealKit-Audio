// mkaudio 시험: 장치 없이(Desc::noDevice) 섞고 render()로 꺼내 본다. 클립은 시험이 만든 WAV 파일.
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

// 16비트 모노 사인파 WAV
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

// 한 프레임(60 fps): update 다음 그 시간만큼 섞어 최댓값을 돌려준다
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

// 이벤트 파일: 읽기·쓰기 왕복, 기본값은 쓰지 않는다, 틀린 줄은 메시지
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
    CHECK(errors == 3 && warnings == 1); // 겹친 이름, 틀린 이름, 틀린 값 / 모르는 키

    const std::string written = mkaudio::writeEvents(f);
    CHECK(written.find("\n@format audio-events 1\n") != std::string::npos);
    CHECK(written.find("pitch ") == std::string::npos); // 기본값은 쓰지 않는다
    CHECK(written.find("volumeRandom 0.15\n") != std::string::npos && written.find("spatial false\n") != std::string::npos);
    mkaudio::EventFile back;
    CHECK(mkaudio::parseEvents(written, back) && mkaudio::writeEvents(back) == written);

    CHECK(!mkaudio::parseEvents("@format table 1\nrow x\n", back)); // 다른 종류의 파일
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

    // 재생하면 들린다, 멈추면 무효
    mkaudio::Handle h = mixer.play2D("beep");
    CHECK(h.valid() && mixer.playing(h));
    CHECK(frame(mixer) > 0.05f && mixer.stats().realVoices == 1 && mixer.stats().playsThisFrame == 1);
    mixer.stop(h);
    CHECK(!mixer.playing(h));
    frame(mixer); // 노드 그래프에 남은 한 덩어리
    CHECK(frame(mixer) < 0.01f);

    // 없는 이벤트, 없는 클립
    CHECK(!mixer.play2D("nope").valid() && culls(mixer, "nope", "missing event") == 1);
    mixer.play2D("missing");
    frame(mixer);
    CHECK(culls(mixer, "missing", "missing clip") == 1);

    // 쿨다운
    mixer.play2D("cool");
    frame(mixer);
    mixer.play2D("cool");
    frame(mixer);
    CHECK(culls(mixer, "cool", "cooldown") == 1);

    // 같은 프레임 묶기, 동시 재생 상한 (끊지 않음)
    for (int i = 0; i < 3; ++i) mixer.play2D("cap");
    frame(mixer);
    CHECK(culls(mixer, "cap", "same frame") == 2);
    mixer.play2D("cap");
    frame(mixer);
    CHECK(culls(mixer, "cap", "max instances") == 1);
    mixer.stopAll();

    // 먼 루프는 가상 목소리, 가까이 가면 다시 섞는다
    mixer.setListener({0, 0, 0}, {0, 0, 1});
    mkaudio::Handle loop = mixer.play("hum", {0, 0, 50});
    frame(mixer);
    CHECK(mixer.playing(loop) && mixer.stats().virtualVoices == 1 && mixer.stats().realVoices == 0);
    mixer.setPosition(loop, {0, 0, 2});
    CHECK(frame(mixer) > 0.01f && mixer.stats().realVoices == 1);
    std::vector<mkaudio::VoiceInfo> voices;
    mixer.voices(voices);
    CHECK(voices.size() == 1 && voices[0].event == "hum" && voices[0].loop && !voices[0].virtualVoice);
    mixer.stop(loop, 0.1f); // 페이드
    for (int i = 0; i < 10; ++i) frame(mixer);
    CHECK(!mixer.playing(loop));

    // 자막: 소리가 들리는지와 무관하게 보인다
    mixer.setMuted(true);
    mixer.play2D("sub");
    CHECK(frame(mixer) == 0.0f);
    CHECK(mixer.subtitles().size() == 1 && mixer.subtitles()[0].key == "subtitle.beep");
    mixer.setMuted(false);

    // 버스
    std::vector<mkaudio::BusInfo> buses;
    mixer.buses(buses);
    CHECK(buses.size() == 5 && buses[0].name == "master" && buses[2].name == "sfx");
    mixer.setBusVolume("sfx", 0.0f);
    CHECK(mixer.busVolume("sfx") == 0.0f);
    mixer.play2D("beep");
    frame(mixer); // 볼륨 변화가 다음 덩어리부터
    CHECK(frame(mixer) < 0.001f);
    // 버스 정의: 부모·믹스 볼륨, 돌고 도는 부모는 master로
    mixer.setBuses({{"ambience", "music", 0.5f}, {"music", "ambience", 1.0f}});
    mixer.buses(buses);
    const auto busNamed = [&](const char* name) {
        for (const mkaudio::BusInfo& b : buses)
            if (b.name == name) return b;
        return mkaudio::BusInfo{};
    };
    CHECK(busNamed("ambience").parent == "music" && busNamed("ambience").mix == 0.5f);
    CHECK(busNamed("music").parent == "master"); // music → ambience → music 고리

    // 정의를 바꾸면 재생 중인 소리는 이름으로 잇고, 없어진 이벤트는 멈춘다
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
