#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// mkaudio: 소리 이벤트 런타임 (MealKit Audio, https://github.com/MealKit-Engine/MealKit-Audio, MIT).
// miniaudio 위에 FMOD·Wwise식 이벤트를 얹는다: 이벤트 이름 + 파라미터로 재생하고, 클립 고르기(무작위·스위치·블렌드 컨테이너),
// 무작위 볼륨·피치, 파라미터 곡선(볼륨·피치·로우패스), 3D 감쇠, 버스, 동시 재생 상한·끊기, 쿨다운, 같은 프레임 묶기,
// 목소리 관리(실제로 섞는 수 상한, 들리지 않는 루프는 가상 목소리)를 런타임이 맡는다.
// 이 라이브러리는 엔진에 기대지 않는다(miniaudio·stb_vorbis·표준 라이브러리만. MealKit 엔진은 tools\check_libs.ps1로 확인한다).
// 이벤트 정의를 어디서 읽는지(MealKit은 assets/sounds.table), 클립 파일을 어디서 찾는지, 로그를 어디에 쓰는지는 쓰는 쪽이 정한다.
// 모든 함수는 한 스레드(게임의 메인 스레드)에서 부른다. 오디오 스레드는 miniaudio가 돌리고, 버스 미터만 원자 값으로 나눈다.
namespace mkaudio {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

enum class Container : uint8_t { Random, Switch, Blend };
enum class Steal : uint8_t { Oldest, Quietest, None };

struct ClipRef {
    std::string name; // 메시지에 쓰는 이름 (예: "sounds/hit_1.wav")
    std::string file; // 열 파일 경로 (비어 있으면 없는 클립: 재생하면 "missing clip")
};

// 소리 이벤트 하나. 왼손 좌표계(+z 앞)의 위치를 받는다
struct EventDef {
    std::string name;
    std::vector<ClipRef> clips;
    Container container = Container::Random;
    std::string param; // 스위치·블렌드·곡선 파라미터 이름
    float paramMin = 0.0f, paramMax = 1.0f;
    float volume = 1.0f, volumeRandom = 0.0f; // 무작위는 ± 비율
    float pitch = 1.0f, pitchRandom = 0.0f;
    float volumeAtMin = 1.0f, volumeAtMax = 1.0f; // 곡선
    float pitchAtMin = 1.0f, pitchAtMax = 1.0f;
    float lowpassAtMin = 0.0f, lowpassAtMax = 0.0f; // Hz, 0 = 끔
    bool spatial = true;
    float minDistance = 2.0f, maxDistance = 40.0f;
    std::string bus = "sfx";
    bool loop = false;
    bool stream = false; // 디스크에서 흘려 읽는다 (음악). 아니면 처음 쓸 때 메모리에 풀어 둔다
    uint32_t priority = 50;
    uint32_t maxInstances = 8;
    Steal steal = Steal::Oldest;
    float cooldown = 0.0f; // 초
    std::string subtitle;  // 자막 키 (비면 없음)
    float subtitleSeconds = 2.5f;
};

struct Param {
    const char* name;
    float value;
};

// 재생 중인 소리 하나. 끝나거나 끊기면 무효가 된다(그 뒤 호출은 무시된다).
struct Handle {
    uint32_t index = 0xFFFFFFFFu;
    uint32_t generation = 0;
    bool valid() const { return index != 0xFFFFFFFFu; }
};

enum class LogLevel : uint8_t { Info, Warn };

struct Desc {
    uint32_t maxVoices = 32; // 실제로 섞는 목소리 수
    bool muted = false;
    // 장치 없이 섞는다 (시험, 오프라인 렌더): 소리는 render()로 꺼낸다. 이때 sampleRate를 쓴다(장치가 있으면 장치 값)
    bool noDevice = false;
    uint32_t sampleRate = 48000;
    uint64_t seed = 0;                                             // 화면용 무작위 (0이면 시각에서)
    std::function<void(LogLevel level, const std::string&)> log; // 없으면 아무것도 쓰지 않는다
};

struct VoiceInfo {
    std::string event;
    bool virtualVoice = false; // 섞지 않고 재생 위치만 센다 (멀거나 작거나 상한 밖)
    bool loop = false;
    float audibility = 0.0f; // 볼륨 × 거리 감쇠
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
    float volume = 1.0f;
    float peak = 0.0f; // 최근 출력 최댓값 (0..1+)
};
// 자막이 있는 소리가 났다 (같은 키는 한 줄로 합치고 시간을 늘린다)
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

    // 기본 출력 장치를 연다. 버스 master(맨 위), music, sfx, ui, voice. 장치가 없으면 false (그 뒤 호출은 아무 일도 하지 않는다)
    bool init(const Desc& desc = {});
    void shutdown();
    bool ready() const;

    // 이벤트 정의를 바꾼다 (처음, 그리고 정의 파일이 바뀔 때). 쿨다운·직전 클립은 이름으로 이어 가고,
    // 재생 중인 소리는 이름으로 새 정의를 찾는다(없어진 이벤트는 멈춘다). 메모리에 풀 클립은 여기서 푼다
    void setEvents(std::vector<EventDef> events);

    // 재생 요청. 실제 시작은 update()에서 (같은 프레임에 같은 이벤트가 여러 번이면 상한만큼, 세기 큰 순)
    Handle play(std::string_view event, Vec3 position, std::initializer_list<Param> params = {});
    Handle play2D(std::string_view event, std::initializer_list<Param> params = {});
    void setParam(Handle handle, std::string_view name, float value);
    void setPosition(Handle handle, Vec3 position);
    void stop(Handle handle, float fadeSeconds = 0.0f);
    void stopAll();
    bool playing(Handle handle) const;
    void setGlobalParam(std::string_view name, float value); // 이벤트가 소리에서 못 찾은 파라미터

    void setListener(Vec3 position, Vec3 forward);
    void setBusVolume(std::string_view bus, float volume);
    float busVolume(std::string_view bus) const;
    void setMuted(bool muted);
    bool muted() const;
    const std::vector<Subtitle>& subtitles() const;

    // 매 프레임 한 번: 재생 요청 처리, 목소리 관리, 끝난 소리 정리 (dt는 초)
    void update(double dt);
    // Desc::noDevice일 때: 섞은 소리 frames 프레임을 out(프레임 × channels()개의 float)에 쓴다. 쓴 프레임 수
    uint64_t render(float* out, uint64_t frames);

    // 확인용 (에디터 패널 등)
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
