#pragma once

#include "mkaudio/Mixer.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// 이벤트 파일 (MealKit 프로젝트에서는 assets/sounds.events). 저작 도구 MealKit Audio가 고치고 게임이 그대로 읽는다(내보내기 없음).
// git에서 병합되는 텍스트다:
//   # MealKit audio events. Edit them in MealKit Audio or by hand.
//   @format audio-events 1
//
//   mixbus music
//   volume 0.8
//
//   event hit
//   clips sounds/hit_1.wav sounds/hit_2.wav
//   container random
//   volumeRandom 0.15
//   bus sfx
// "event <이름>"·"mixbus <이름>"이 블록을 연다("bus"는 이벤트가 나가는 버스를 정하는 키라 버스 정의는 mixbus), 그 아래 "키 값" 줄이 이어진다. 적지 않은 키는 기본값(EventDef·BusDef의 초기값)이다.
// 이벤트 키는 EventDef 필드 이름 그대로, 클립은 공백으로 나눈 경로(쓰는 쪽 기준, MealKit은 assets 기준), container는 random·switch·blend,
// steal은 oldest·quietest·none, 참거짓은 true·false. 쓰기는 정해진 키 순서로 기본값과 다른 키만 쓴다.
// 형식을 바꾸면 kEventsFormatVersion을 올리고 읽는 쪽에 예전 버전 migrate를 더한다(MealKit 도구 사이 약속의 형식 줄).
namespace mkaudio {

inline constexpr const char* kEventsFormatKind = "audio-events";
inline constexpr uint32_t kEventsFormatVersion = 1;

struct EventFile {
    std::vector<BusDef> buses;
    std::vector<EventDef> events; // 파일 순서
};

struct ParseMessage {
    uint32_t line = 0;
    bool error = false; // false = 경고 (아는 것만 읽고 계속)
    std::string text;
};

// 글자를 읽는다. 틀린 줄은 messages에 남기고 건너뛴다(형식 줄이 아예 다른 종류면 false). 클립의 file은 비어 있다(resolveClips)
bool parseEvents(std::string_view text, EventFile& out, std::vector<ParseMessage>* messages = nullptr);
std::string writeEvents(const EventFile& file);

bool loadEventFile(const std::string& path, EventFile& out, std::vector<ParseMessage>* messages = nullptr);
bool saveEventFile(const std::string& path, const EventFile& file); // 같은 폴더의 임시 파일에 쓰고 바꿔 넣는다

// 클립 이름(파일에 적힌 경로) → 열 파일 경로. 찾지 못하면 빈 문자열을 돌려준다
void resolveClips(EventFile& file, const std::function<std::string(const std::string& name)>& resolve);

// 이름 확인: 글자·숫자·_ - . 만 (파일의 "event <이름>" 줄과 게임 코드의 play("이름"))
bool validName(std::string_view name);

const char* containerName(Container c);
const char* stealName(Steal s);

} // namespace mkaudio
