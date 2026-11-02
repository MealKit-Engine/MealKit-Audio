#pragma once

#include "mkaudio/Events.h"
#include "mkaudio/Mixer.h"

#include <mkcontract/Client.h>
#include <mkcontract/Json.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// MealKit Audio: authoring tool for sound designers. Edits the project's assets/sounds.events (read by the game as is),
// previews with the runtime (mkaudio::Mixer), and plays in a running game/editor and watches state over the live connection.
namespace mkaudio::tool {

// Live connection (MealKit tool contract, MealKit-Contract mkcontract_client)
using mkcontract::Json;
using mkcontract::live::findInstances;
using mkcontract::live::HttpResult;
using mkcontract::live::Instance;
using mkcontract::live::request;
using mkcontract::live::urlEncode;
inline constexpr uint32_t kLiveProtocol = mkcontract::live::kProtocol; // protocol in mealkit-tool.txt

class App {
public:
    bool init(const std::string& projectDir); // project picker screen if empty
    void shutdown();
    // Every frame (between ImGui NewFrame and Render). When the window is about to close, requestQuit(); canQuit() if nothing is edited or the user decides
    void frame(double dt);
    void requestQuit() { m_quitAsked = true; }
    bool canQuit() const { return m_quitConfirmed; }
    std::string title() const;
    // Project passed from another thread (folder picker dialog)
    void setPendingProject(const std::string& dir);
    void select(const std::string& event); // selects the event with this name (command line --select)
    void* window = nullptr; // SDL_Window* (dialog parent)

private:
    // Project/file
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
    void snapshot(); // undo record
    void undo(bool redo);

    // Screen
    void drawMenu();
    void drawStart();
    void drawEventList();
    void drawEventEditor();
    void drawBuses();
    void drawMusic();
    void drawMix();
    void drawPreview();
    void drawLive();
    void drawQuitDialog();
    void layoutDocks(unsigned dockspace);

    // Live connection
    void refreshLive(bool force);
    void playInGame(const EventDef& e);
    void reloadInGames();

    std::string m_project; // UTF-8
    EventFile m_file;
    std::string m_savedText;    // contents last read or written (edited = the current text differs)
    std::string m_appliedText;  // contents put into the preview mixer
    std::filesystem::file_time_type m_fileTime{};
    double m_fileCheck = 0.0;
    bool m_externalChange = false;
    std::vector<ParseMessage> m_messages;
    std::vector<std::string> m_clipFiles; // paths relative to assets (.wav .ogg)
    std::string m_status;
    double m_statusTime = 0.0;

    std::deque<std::string> m_undo, m_redo;
    std::string m_lastSnapshot;

    int m_selected = -1;
    int m_previewFor = -1; // event the preview parameters were set for (reset to the range start when the selection changes)
    char m_filter[64] = {};
    char m_rename[64] = {};
    int m_renaming = -1;

    Mixer m_mixer;
    bool m_audio = false;
    Handle m_preview;
    float m_param = 0.0f;
    float m_distance = 3.0f;
    float m_occlusion = 0.0f;
    float m_stemParam = 0.0f;
    ReverbParams m_reverb;
    int m_musicSelected = -1;
    char m_musicRename[64] = {};
    int m_musicRenaming = -1;
    char m_snapRename[64] = {};
    int m_snapRenaming = -1;

    std::vector<Instance> m_live;
    std::vector<bool> m_liveOn;
    std::string m_liveState; // audio/state of the selected game (JSON)
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
