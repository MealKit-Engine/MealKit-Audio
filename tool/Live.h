#pragma once

#include "Json.h"

#include <cstdint>
#include <string>
#include <vector>

// 라이브 연결 클라이언트 (MealKit 도구 사이 약속: 프로젝트 build/live/<종류>-<pid>.json → http://127.0.0.1:<port>/<토큰>/...).
// 이 컴퓨터 안의 짧은 요청이라 메인 스레드에서 짧은 시간 제한으로 막고 기다린다.
namespace mkaudio::tool {

struct Instance {
    std::string kind; // "game", "editor"
    std::string project, engine, url, token;
    uint32_t pid = 0;
    uint16_t port = 0;
    uint32_t protocol = 0;
};

struct HttpResult {
    int status = 0; // 0 = 연결하지 못했다
    std::string body;
};

inline constexpr uint32_t kLiveProtocol = 1; // 엔진 live::kProtocol, mealkit-tool.txt의 protocol

// 프로젝트의 접속 정보 파일들 (파일이 남아 있어도 프로세스가 없을 수 있다: 요청이 실패하면 없는 것으로 본다)
std::vector<Instance> findInstances(const std::string& projectDir);

// path는 토큰 뒤 경로 ("audio/state"). body는 POST 본문
HttpResult request(const Instance& to, const char* method, const std::string& path, const std::string& body = {}, int timeoutMs = 300);

// 폼 본문용 (a=b&c=d): 글자·숫자·-_.~ 밖은 %XX
std::string urlEncode(const std::string& text);

} // namespace mkaudio::tool
