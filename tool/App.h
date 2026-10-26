#pragma once

#include "Live.h"

#include "mkaudio/Events.h"
#include "mkaudio/Mixer.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// MealKit Audio: 사운드 디자이너용 저작 도구. 프로젝트의 assets/sounds.events를 고치고(게임이 그대로 읽는다),
// 런타임(mkaudio::Mixer)으로 미리 듣고, 라이브 연결로 실행 중인 게임·에디터에서 재생하고 상태를 본다.
namespace mkaudio::tool {

class App {
public:
    bool init(const std::string& projectDir); // 비면 프로젝트 고르기 화면
    void shutdown();
    // 매 프레임 (ImGui NewFrame과 Render 사이). 창을 닫으려 하면 requestQuit(), 고친 것이 없거나 사용자가 정하면 canQuit()
    void frame(double dt);
    void requestQuit() { m_quitAsked = true; }
    bool canQuit() const { return m_quitConfirmed; }
    std::string title() const;
    // 다른 스레드(폴더 고르기 대화상자)가 넘긴 프로젝트
    void setPendingProject(const std::string& dir);
    void select(const std::string& event); // 이 이름의 이벤트를 고른다 (명령줄 --select)
    void* window = nullptr; // SDL_Window* (대화상자 부모)

private:
    // 프로젝트·파일
    bool openProject(const std::string& dir);
    std::string assetsDir() const;
    std::string eventsPath() const;
    std::string resolveClip(const std::string& name) const;
    void scanClips();
    bool load();
    bool save();
    void importTable();
    void checkExternalChange();
    void applyToMixer();
    void snapshot(); // 되돌리기 기록
    void undo(bool redo);

    // 화면
    void drawMenu();
    void drawStart();
    void drawEventList();
    void drawEventEditor();
    void drawBuses();
    void drawPreview();
    void drawLive();
    void drawQuitDialog();
    void layoutDocks(unsigned dockspace);

    // 라이브 연결
    void refreshLive(bool force);
    void playInGame(const EventDef& e);
    void reloadInGames();

    std::string m_project; // UTF-8
    EventFile m_file;
    std::string m_savedText;    // 마지막으로 읽거나 쓴 내용 (고쳤는지 = 지금 글자가 다르다)
    std::string m_appliedText;  // 미리 듣기 믹서에 넣은 내용
    std::filesystem::file_time_type m_fileTime{};
    double m_fileCheck = 0.0;
    bool m_externalChange = false;
    std::vector<ParseMessage> m_messages;
    std::vector<std::string> m_clipFiles; // assets 기준 경로 (.wav .ogg)
    std::string m_status;
    double m_statusTime = 0.0;

    std::deque<std::string> m_undo, m_redo;
    std::string m_lastSnapshot;

    int m_selected = -1;
    int m_previewFor = -1; // 미리 듣기 파라미터를 맞춘 이벤트 (고른 것이 바뀌면 범위 시작으로)
    char m_filter[64] = {};
    char m_rename[64] = {};
    int m_renaming = -1;

    Mixer m_mixer;
    bool m_audio = false;
    Handle m_preview;
    float m_param = 0.0f;
    float m_distance = 3.0f;

    std::vector<Instance> m_live;
    std::vector<bool> m_liveOn;
    std::string m_liveState; // 고른 게임의 audio/state (JSON)
    Json m_liveJson;
    double m_liveCheck = 0.0, m_stateCheck = 0.0;
    std::string m_liveError;

    double m_time = 0.0;
    bool m_layoutDone = false;
    bool m_quitAsked = false, m_quitConfirmed = false;
    std::string m_pendingProject;
    std::mutex m_pendingMutex;
};

} // namespace mkaudio::tool
