#include "App.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace mkaudio::tool {
namespace {

constexpr const char* kEventsFile = "sounds.events"; // assets 기준
constexpr const char* kOldTable = "sounds.table";    // MealKit 엔진 0.5의 형식 (불러와 옮긴다)
constexpr size_t kUndoDepth = 200;
const char* const kStandardBuses[] = {"master", "music", "sfx", "ui", "voice"}; // 런타임이 처음부터 만드는 버스

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }

std::string utf8Of(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool readFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}

// 곡선 하나: 파라미터 min(왼쪽)~max(오른쪽)에서 값 atMin~atMax 직선. 끝점을 위아래로 끌어 바꾼다. t = 미리 듣기 위치(0..1)
bool curveEditor(const char* id, float& atMin, float& atMax, float lo, float hi, float t, const char* format) {
    bool changed = false;
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 70.0f * ImGui::GetStyle().FontScaleDpi);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    const float pad = 8.0f;
    auto yOf = [&](float v) { return p1.y - pad - (std::clamp(v, lo, hi) - lo) / (hi - lo) * (size.y - 2 * pad); };
    const ImVec2 a(p0.x + pad, yOf(atMin)), b(p1.x - pad, yOf(atMax));
    dl->AddLine(a, b, ImGui::GetColorU32(ImGuiCol_PlotLines), 2.0f);
    const float tx = a.x + (b.x - a.x) * t;
    dl->AddLine(ImVec2(tx, p0.y + 2), ImVec2(tx, p1.y - 2), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    for (int i = 0; i < 2; ++i) {
        float& v = i == 0 ? atMin : atMax;
        const ImVec2 c = i == 0 ? a : b;
        ImGui::SetCursorScreenPos(ImVec2(c.x - 7, c.y - 7));
        ImGui::PushID(id);
        ImGui::InvisibleButton(i == 0 ? "##min" : "##max", ImVec2(14, 14));
        const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (active) {
            const float delta = -ImGui::GetIO().MouseDelta.y / (size.y - 2 * pad) * (hi - lo);
            v = std::clamp(v + delta, lo, hi);
            changed = true;
        }
        dl->AddCircleFilled(c, active || hovered ? 6.0f : 4.5f, ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab));
        if (hovered || active) ImGui::SetTooltip(format, static_cast<double>(v));
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 4));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

// 거리 감쇠 그림: min까지 1, max에서 0 (런타임과 같은 직선 모델). d = 미리 듣기 거리
void attenuationPlot(float minDistance, float maxDistance, float d) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 60.0f * ImGui::GetStyle().FontScaleDpi);
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size.x, p0.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    const float range = std::max(maxDistance * 1.2f, 1.0f), pad = 6.0f;
    auto xOf = [&](float v) { return p0.x + pad + v / range * (size.x - 2 * pad); };
    const float top = p0.y + pad, bottom = p1.y - pad;
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_PlotLines);
    dl->AddLine(ImVec2(xOf(0), top), ImVec2(xOf(minDistance), top), color, 2.0f);
    dl->AddLine(ImVec2(xOf(minDistance), top), ImVec2(xOf(maxDistance), bottom), color, 2.0f);
    dl->AddLine(ImVec2(xOf(maxDistance), bottom), ImVec2(xOf(range), bottom), color, 2.0f);
    dl->AddLine(ImVec2(xOf(std::min(d, range)), p0.y + 2), ImVec2(xOf(std::min(d, range)), p1.y - 2), ImGui::GetColorU32(ImGuiCol_TextDisabled));
    ImGui::Dummy(size);
}

bool knownBus(const EventFile& f, const std::string& name) {
    for (const char* b : kStandardBuses)
        if (name == b) return true;
    for (const BusDef& b : f.buses)
        if (b.name == name) return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------- 시작·끝

bool App::init(const std::string& projectDir) {
    Desc d;
    d.log = [this](LogLevel, const std::string& text) {
        m_status = "Audio: " + text;
        m_statusTime = m_time;
    };
    m_audio = m_mixer.init(d);
    if (!m_audio) m_status = "No audio output device: preview is off";
    if (!projectDir.empty()) openProject(projectDir);
    return true;
}

void App::shutdown() { m_mixer.shutdown(); }

std::string App::title() const {
    if (m_project.empty()) return "MealKit Audio";
    const bool dirty = writeEvents(m_file) != m_savedText;
    return std::string(dirty ? "* " : "") + utf8Of(pathOf(m_project).filename()) + " - MealKit Audio";
}

void App::setPendingProject(const std::string& dir) {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProject = dir;
}

void App::select(const std::string& event) {
    for (size_t i = 0; i < m_file.events.size(); ++i)
        if (m_file.events[i].name == event) m_selected = static_cast<int>(i);
}

bool App::openProject(const std::string& dir) {
    const std::filesystem::path root = pathOf(dir);
    std::error_code ec;
    if (!std::filesystem::is_directory(root / "assets", ec)) {
        m_status = "Not a MealKit project (no assets folder): " + dir;
        m_statusTime = m_time;
        return false;
    }
    m_project = utf8Of(std::filesystem::absolute(root, ec));
    m_selected = -1;
    m_undo.clear();
    m_redo.clear();
    load();
    scanClips();
    refreshLive(true);
    return true;
}

std::string App::assetsDir() const { return utf8Of(pathOf(m_project) / "assets"); }
std::string App::eventsPath() const { return utf8Of(pathOf(m_project) / "assets" / kEventsFile); }

std::string App::resolveClip(const std::string& name) const {
    std::error_code ec;
    const std::filesystem::path p = pathOf(m_project) / "assets" / pathOf(name);
    return std::filesystem::is_regular_file(p, ec) ? utf8Of(p) : std::string();
}

void App::scanClips() {
    m_clipFiles.clear();
    std::error_code ec;
    const std::filesystem::path root = pathOf(assetsDir());
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator();
         it.increment(ec)) {
        std::string ext = utf8Of(it->path().extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (ext != ".wav" && ext != ".ogg") continue;
        std::string rel = utf8Of(std::filesystem::relative(it->path(), root, ec));
        std::replace(rel.begin(), rel.end(), '\\', '/');
        m_clipFiles.push_back(rel);
    }
    std::sort(m_clipFiles.begin(), m_clipFiles.end());
}

bool App::load() {
    m_messages.clear();
    m_externalChange = false;
    std::error_code ec;
    const std::filesystem::path path = pathOf(eventsPath());
    if (!std::filesystem::exists(path, ec)) {
        m_file = {};
        m_savedText = writeEvents(m_file);
        m_fileTime = {};
        m_status = "No assets/sounds.events yet: add events and save to create it";
    } else {
        loadEventFile(eventsPath(), m_file, &m_messages);
        m_savedText = writeEvents(m_file);
        m_fileTime = std::filesystem::last_write_time(path, ec);
        m_status = "Loaded assets/sounds.events: " + std::to_string(m_file.events.size()) + " event(s)";
    }
    m_statusTime = m_time;
    m_lastSnapshot = m_savedText;
    if (m_selected >= static_cast<int>(m_file.events.size())) m_selected = -1;
    if (m_selected < 0 && !m_file.events.empty()) m_selected = 0;
    applyToMixer();
    return true;
}

bool App::save() {
    if (m_project.empty()) return false;
    if (!saveEventFile(eventsPath(), m_file)) {
        m_status = "Cannot write " + eventsPath();
        m_statusTime = m_time;
        return false;
    }
    std::error_code ec;
    m_savedText = writeEvents(m_file);
    m_fileTime = std::filesystem::last_write_time(pathOf(eventsPath()), ec);
    m_externalChange = false;
    m_status = "Saved assets/sounds.events";
    m_statusTime = m_time;
    reloadInGames(); // 실행 중인 게임·에디터는 파일 시각으로도 다시 읽지만, 바로 듣게 알린다
    return true;
}

// MealKit 0.5의 assets/sounds.table ("row <이름>" + 같은 키) → 이벤트
void App::importTable() {
    std::string text;
    if (!readFile(pathOf(m_project) / "assets" / kOldTable, text)) return;
    std::string converted;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("@format", 0) == 0 || line.rfind("#", 0) == 0) continue;
        if (line.rfind("row ", 0) == 0) line = "event " + line.substr(4);
        converted += line + "\n";
    }
    EventFile imported;
    m_messages.clear();
    parseEvents(converted, imported, &m_messages);
    m_file.events = imported.events;
    m_selected = m_file.events.empty() ? -1 : 0;
    m_status = "Imported " + std::to_string(m_file.events.size()) + " event(s) from assets/sounds.table - save to write sounds.events";
    m_statusTime = m_time;
}

void App::checkExternalChange() {
    if (m_project.empty() || m_time < m_fileCheck) return;
    m_fileCheck = m_time + 0.5;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(pathOf(eventsPath()), ec);
    if (ec || t == m_fileTime) return;
    if (writeEvents(m_file) == m_savedText) load(); // 고친 것이 없으면 그냥 다시 읽는다
    else m_externalChange = true;
}

void App::applyToMixer() {
    const std::string text = writeEvents(m_file);
    if (!m_audio || text == m_appliedText) return;
    m_appliedText = text;
    EventFile resolved = m_file;
    resolveClips(resolved, [this](const std::string& name) { return resolveClip(name); });
    m_mixer.setBuses(resolved.buses);
    m_mixer.setEvents(resolved.events);
}

void App::snapshot() {
    if (ImGui::IsAnyItemActive()) return; // 끄는 중에는 한 번만 (놓을 때)
    const std::string text = writeEvents(m_file);
    if (text == m_lastSnapshot) return;
    m_undo.push_back(m_lastSnapshot);
    if (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
    m_lastSnapshot = text;
}

void App::undo(bool redo) {
    auto& from = redo ? m_redo : m_undo;
    auto& to = redo ? m_undo : m_redo;
    if (from.empty()) return;
    to.push_back(m_lastSnapshot);
    m_lastSnapshot = from.back();
    from.pop_back();
    parseEvents(m_lastSnapshot, m_file);
    if (m_selected >= static_cast<int>(m_file.events.size())) m_selected = static_cast<int>(m_file.events.size()) - 1;
}

// ---------------------------------------------------------------- 매 프레임

void App::frame(double dt) {
    m_time += dt;
    {
        std::string pending;
        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);
            pending.swap(m_pendingProject);
        }
        if (!pending.empty()) openProject(pending);
    }
    if (m_audio) m_mixer.update(dt);
    checkExternalChange();
    refreshLive(false);

    const ImGuiIO& io = ImGui::GetIO();
    if (!m_project.empty() && io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) save();
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) undo(io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) undo(true);
    }

    drawMenu();
    const ImGuiID dock = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    if (m_project.empty()) {
        drawStart();
    } else {
        if (!m_layoutDone) layoutDocks(dock);
        drawEventList();
        drawEventEditor();
        drawBuses();
        drawPreview();
        drawLive();
        applyToMixer();
        snapshot();
    }
    drawQuitDialog();
}

void App::layoutDocks(unsigned dockspace) {
    m_layoutDone = true;
    if (ImGui::DockBuilderGetNode(dockspace) && ImGui::DockBuilderGetNode(dockspace)->IsSplitNode()) return; // imgui.ini에 배치가 있다
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left = 0, center = 0, right = 0, rightBottom = 0;
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.22f, &left, &center);
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.36f, &right, &center);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.45f, &rightBottom, &right);
    ImGui::DockBuilderDockWindow("Events", left);
    ImGui::DockBuilderDockWindow("Event", center);
    ImGui::DockBuilderDockWindow("Preview", right);
    ImGui::DockBuilderDockWindow("Buses", rightBottom);
    ImGui::DockBuilderDockWindow("Live", rightBottom);
    ImGui::DockBuilderFinish(dockspace);
}

void App::drawMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Project Folder...")) {
            SDL_ShowOpenFolderDialog(
                [](void* user, const char* const* list, int) {
                    if (list && list[0]) static_cast<App*>(user)->setPendingProject(list[0]);
                },
                this, static_cast<SDL_Window*>(window), m_project.empty() ? nullptr : m_project.c_str(), false);
        }
        if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_project.empty())) save();
        if (ImGui::MenuItem("Reload from Disk", nullptr, false, !m_project.empty())) load();
        std::error_code ec;
        const bool hasTable = !m_project.empty() && std::filesystem::exists(pathOf(m_project) / "assets" / kOldTable, ec);
        if (ImGui::MenuItem("Import assets/sounds.table", nullptr, false, hasTable)) importTable();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit", !m_project.empty())) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_undo.empty())) undo(false);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_redo.empty())) undo(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Rescan Clips")) scanClips();
        ImGui::EndMenu();
    }
    if (!m_status.empty() && m_time - m_statusTime < 8.0) {
        ImGui::SameLine(ImGui::GetWindowWidth() * 0.45f);
        ImGui::TextDisabled("%s", m_status.c_str());
    }
    ImGui::EndMainMenuBar();
}

void App::drawStart() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##start", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("MealKit Audio");
    ImGui::TextDisabled("Sound events for a MealKit project (assets/sounds.events)");
    ImGui::Spacing();
    if (ImGui::Button("Open Project Folder...", ImVec2(260, 0)))
        SDL_ShowOpenFolderDialog(
            [](void* user, const char* const* list, int) {
                if (list && list[0]) static_cast<App*>(user)->setPendingProject(list[0]);
            },
            this, static_cast<SDL_Window*>(window), nullptr, false);
    ImGui::TextDisabled("The MealKit editor opens this tool from Tools > MealKit Audio.");
    if (!m_status.empty()) ImGui::TextDisabled("%s", m_status.c_str());
    ImGui::End();
}

// ---------------------------------------------------------------- 이벤트 목록

void App::drawEventList() {
    ImGui::Begin("Events");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter", m_filter, sizeof(m_filter));
    if (ImGui::Button("+ Event")) {
        EventDef e;
        int n = 1;
        do e.name = "event" + std::to_string(n++);
        while (std::any_of(m_file.events.begin(), m_file.events.end(), [&](const EventDef& o) { return o.name == e.name; }));
        m_file.events.push_back(e);
        m_selected = static_cast<int>(m_file.events.size()) - 1;
    }
    ImGui::SameLine();
    const bool has = m_selected >= 0 && m_selected < static_cast<int>(m_file.events.size());
    ImGui::BeginDisabled(!has);
    if (ImGui::Button("Duplicate")) {
        EventDef e = m_file.events[static_cast<size_t>(m_selected)];
        e.name += "_copy";
        m_file.events.insert(m_file.events.begin() + m_selected + 1, e);
        ++m_selected;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        m_file.events.erase(m_file.events.begin() + m_selected);
        m_selected = std::min(m_selected, static_cast<int>(m_file.events.size()) - 1);
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (m_externalChange) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "sounds.events changed on disk");
        if (ImGui::SmallButton("Reload (lose my changes)")) load();
        ImGui::Separator();
    }
    ImGui::BeginChild("##list");
    for (int i = 0; i < static_cast<int>(m_file.events.size()); ++i) {
        const EventDef& e = m_file.events[static_cast<size_t>(i)];
        if (m_filter[0] && e.name.find(m_filter) == std::string::npos) continue;
        bool missing = false;
        for (const ClipRef& c : e.clips) missing = missing || resolveClip(c.name).empty();
        if (missing || e.clips.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        if (ImGui::Selectable(e.name.c_str(), i == m_selected)) m_selected = i;
        if (missing || e.clips.empty()) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_selected = m_previewFor = i;
            m_param = e.paramMin;
            m_preview = m_mixer.play(e.name, {0, 0, m_distance}, {{e.param.c_str(), m_param}});
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- 이벤트 편집

void App::drawEventEditor() {
    ImGui::Begin("Event");
    if (m_selected < 0 || m_selected >= static_cast<int>(m_file.events.size())) {
        ImGui::TextDisabled("Select or add an event.");
        if (!m_messages.empty()) {
            ImGui::SeparatorText("While reading sounds.events");
            for (const ParseMessage& m : m_messages) ImGui::TextDisabled("line %u: %s%s", m.line, m.error ? "error: " : "", m.text.c_str());
        }
        ImGui::End();
        return;
    }
    EventDef& e = m_file.events[static_cast<size_t>(m_selected)];

    // 이름 (game code: ctx.audio.play("name", ...))
    if (m_renaming != m_selected) {
        std::snprintf(m_rename, sizeof(m_rename), "%s", e.name.c_str());
        m_renaming = m_selected;
    }
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputText("Name", m_rename, sizeof(m_rename), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
        const std::string name = m_rename;
        const bool taken = std::any_of(m_file.events.begin(), m_file.events.end(), [&](const EventDef& o) { return &o != &e && o.name == name; });
        if (validName(name) && !taken) e.name = name;
        else {
            m_status = taken ? "Name already used: " + name : "Names use letters, digits, _ - . only";
            m_statusTime = m_time;
            std::snprintf(m_rename, sizeof(m_rename), "%s", e.name.c_str());
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("game code: play(\"%s\")", e.name.c_str());

    // ---- 클립
    ImGui::SeparatorText("Clips");
    int container = static_cast<int>(e.container);
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("Container", &container, "random\0switch\0blend\0")) e.container = static_cast<Container>(container);
    ImGui::SameLine();
    ImGui::TextDisabled(e.container == Container::Random   ? "one clip at random, never the same twice"
                        : e.container == Container::Switch ? "the parameter picks clip 0, 1, 2..."
                                                           : "all clips crossfaded along the parameter");
    int removeAt = -1, moveUp = -1;
    for (size_t i = 0; i < e.clips.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool missing = resolveClip(e.clips[i].name).empty();
        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("^")) moveUp = static_cast<int>(i);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (missing) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%zu  %s (missing)", i, e.clips[i].name.c_str());
        else ImGui::Text("%zu  %s", i, e.clips[i].name.c_str());
        ImGui::PopID();
    }
    if (removeAt >= 0) e.clips.erase(e.clips.begin() + removeAt);
    if (moveUp > 0) std::swap(e.clips[static_cast<size_t>(moveUp)], e.clips[static_cast<size_t>(moveUp) - 1]);
    ImGui::SetNextItemWidth(320);
    if (ImGui::BeginCombo("##addclip", "+ Add clip from assets/", ImGuiComboFlags_HeightLarge)) {
        static char clipFilter[64] = {};
        ImGui::InputTextWithHint("##cf", "Filter", clipFilter, sizeof(clipFilter));
        for (const std::string& f : m_clipFiles) {
            if (clipFilter[0] && f.find(clipFilter) == std::string::npos) continue;
            if (ImGui::Selectable(f.c_str())) e.clips.push_back({f, {}});
        }
        if (m_clipFiles.empty()) ImGui::TextDisabled("No .wav or .ogg files under assets/");
        ImGui::EndCombo();
    }

    // ---- 소리 크기·높이
    ImGui::SeparatorText("Volume and pitch");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Volume", &e.volume, 0.0f, 2.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("+/- random##v", &e.volumeRandom, 0.0f, 0.5f, "%.2f");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderFloat("Pitch", &e.pitch, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::SliderFloat("+/- random##p", &e.pitchRandom, 0.0f, 0.5f, "%.2f");

    // ---- 파라미터와 곡선
    ImGui::SeparatorText("Parameter");
    char param[64];
    std::snprintf(param, sizeof(param), "%s", e.param.c_str());
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputText("Name##param", param, sizeof(param))) e.param = param;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::DragFloatRange2("Range", &e.paramMin, &e.paramMax, 0.05f);
    const float lo = std::min(e.paramMin, e.paramMax), hi = std::max(e.paramMin, e.paramMax);
    const float t = (e.param.empty() || hi == lo) ? 0.0f : std::clamp((m_param - e.paramMin) / (e.paramMax - e.paramMin), 0.0f, 1.0f);
    if (e.param.empty()) {
        ImGui::TextDisabled("No parameter: the curves below are not used. Game code passes it as {{\"name\", value}}.");
    } else {
        ImGui::Text("Volume curve (%.2f -> %.2f)", e.volumeAtMin, e.volumeAtMax);
        curveEditor("vol", e.volumeAtMin, e.volumeAtMax, 0.0f, 2.0f, t, "%.2f");
        ImGui::Text("Pitch curve (%.2f -> %.2f)", e.pitchAtMin, e.pitchAtMax);
        curveEditor("pitch", e.pitchAtMin, e.pitchAtMax, 0.25f, 2.0f, t, "%.2f");
        ImGui::Text("Low-pass curve (%.0f -> %.0f Hz, 0 = off)", e.lowpassAtMin, e.lowpassAtMax);
        curveEditor("lpf", e.lowpassAtMin, e.lowpassAtMax, 0.0f, 20000.0f, t, "%.0f Hz");
    }

    // ---- 3D
    ImGui::SeparatorText("3D");
    ImGui::Checkbox("Positioned in the world", &e.spatial);
    if (e.spatial) {
        ImGui::SetNextItemWidth(260);
        ImGui::DragFloatRange2("Distance (full, silent)", &e.minDistance, &e.maxDistance, 0.1f, 0.0f, 1000.0f, "%.1f m", "%.1f m");
        attenuationPlot(e.minDistance, e.maxDistance, m_distance);
    }

    // ---- 재생
    ImGui::SeparatorText("Playback");
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("Bus", e.bus.c_str())) {
        for (const char* b : kStandardBuses)
            if (std::string(b) != "master" && ImGui::Selectable(b, e.bus == b)) e.bus = b;
        for (const BusDef& b : m_file.buses)
            if (ImGui::Selectable(b.name.c_str(), e.bus == b.name)) e.bus = b.name;
        ImGui::EndCombo();
    }
    if (!knownBus(m_file, e.bus)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "not defined (made under master)");
    }
    ImGui::Checkbox("Loop", &e.loop);
    ImGui::SameLine();
    ImGui::Checkbox("Stream from disk (music)", &e.stream);
    int priority = static_cast<int>(e.priority), maxInstances = static_cast<int>(e.maxInstances);
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Priority", &priority, 0, 100)) e.priority = static_cast<uint32_t>(priority);
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("Max at once", &maxInstances, 1, 32)) e.maxInstances = static_cast<uint32_t>(maxInstances);
    ImGui::SameLine();
    int steal = static_cast<int>(e.steal);
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("When full", &steal, "stop oldest\0stop quietest\0don't play\0")) e.steal = static_cast<Steal>(steal);
    ImGui::SetNextItemWidth(160);
    ImGui::DragFloat("Cooldown", &e.cooldown, 0.01f, 0.0f, 60.0f, "%.2f s");

    // ---- 자막
    ImGui::SeparatorText("Subtitle");
    char subtitle[96];
    std::snprintf(subtitle, sizeof(subtitle), "%s", e.subtitle.c_str());
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputTextWithHint("String key", "e.g. sub.door", subtitle, sizeof(subtitle))) e.subtitle = subtitle;
    if (!e.subtitle.empty()) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Shown for", &e.subtitleSeconds, 0.05f, 0.2f, 30.0f, "%.1f s");
    }

    if (!m_messages.empty()) {
        ImGui::SeparatorText("While reading sounds.events");
        for (const ParseMessage& m : m_messages) ImGui::TextDisabled("line %u: %s%s", m.line, m.error ? "error: " : "", m.text.c_str());
    }
    ImGui::End();
}

// ---------------------------------------------------------------- 버스

void App::drawBuses() {
    ImGui::Begin("Buses");
    std::vector<BusInfo> live;
    if (m_audio) m_mixer.buses(live);
    auto peakOf = [&](const std::string& name) {
        for (const BusInfo& b : live)
            if (b.name == name) return b.peak;
        return 0.0f;
    };
    int removeAt = -1;
    if (ImGui::BeginTable("##buses", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Bus");
        ImGui::TableSetupColumn("Parent");
        ImGui::TableSetupColumn("Mix volume");
        ImGui::TableSetupColumn("Level");
        ImGui::TableHeadersRow();
        auto meter = [&](const std::string& name) {
            const float p = peakOf(name);
            char overlay[16];
            std::snprintf(overlay, sizeof(overlay), "%.2f", p);
            ImGui::ProgressBar(std::min(p, 1.0f), ImVec2(-FLT_MIN, 0), overlay);
        };
        for (const char* b : kStandardBuses) {
            if (std::any_of(m_file.buses.begin(), m_file.buses.end(), [&](const BusDef& d) { return d.name == b; })) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b);
            ImGui::TableNextColumn();
            ImGui::TextDisabled(std::string(b) == "master" ? "-" : "master");
            ImGui::TableNextColumn();
            ImGui::TextDisabled("1.00");
            ImGui::TableNextColumn();
            meter(b);
        }
        for (size_t i = 0; i < m_file.buses.size(); ++i) {
            BusDef& b = m_file.buses[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
            ImGui::SameLine();
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::TableNextColumn();
            if (b.name != "master") {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##parent", b.parent.c_str())) {
                    for (const char* s : kStandardBuses)
                        if (b.name != s && ImGui::Selectable(s, b.parent == s)) b.parent = s;
                    for (const BusDef& o : m_file.buses)
                        if (o.name != b.name && !std::any_of(std::begin(kStandardBuses), std::end(kStandardBuses), [&](const char* s) { return o.name == s; }) &&
                            ImGui::Selectable(o.name.c_str(), b.parent == o.name))
                            b.parent = o.name;
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##mix", &b.volume, 0.0f, 1.5f, "%.2f");
            ImGui::TableNextColumn();
            meter(b.name);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (removeAt >= 0) m_file.buses.erase(m_file.buses.begin() + removeAt);
    static char name[48] = {};
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##newbus", "bus name", name, sizeof(name));
    ImGui::SameLine();
    const bool exists = std::any_of(m_file.buses.begin(), m_file.buses.end(), [&](const BusDef& b) { return b.name == name; });
    ImGui::BeginDisabled(!validName(name) || exists);
    if (ImGui::Button("+ Bus")) {
        BusDef b;
        b.name = name;
        m_file.buses.push_back(b);
        name[0] = 0;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Standard buses can be added by name to change their mix volume or parent.");
    ImGui::End();
}

// ---------------------------------------------------------------- 미리 듣기

void App::drawPreview() {
    ImGui::Begin("Preview");
    if (!m_audio) {
        ImGui::TextDisabled("No audio output device.");
        ImGui::End();
        return;
    }
    const EventDef* e = (m_selected >= 0 && m_selected < static_cast<int>(m_file.events.size())) ? &m_file.events[static_cast<size_t>(m_selected)] : nullptr;
    if (e && m_previewFor != m_selected) {
        m_previewFor = m_selected;
        m_param = e->paramMin;
    }
    ImGui::BeginDisabled(!e);
    if (ImGui::Button("Play  (double-click an event)", ImVec2(-FLT_MIN, 0)) && e) {
        m_preview = m_mixer.play(e->name, {0, 0, m_distance}, {{e->param.c_str(), m_param}});
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Stop", ImVec2(-FLT_MIN, 0))) m_mixer.stop(m_preview, 0.1f);
    if (ImGui::Button("Stop all")) m_mixer.stopAll();
    if (e && !e->param.empty()) {
        const float lo = std::min(e->paramMin, e->paramMax), hi = std::max(e->paramMin, e->paramMax);
        ImGui::SetNextItemWidth(-120);
        if (ImGui::SliderFloat(e->param.c_str(), &m_param, lo, hi)) m_mixer.setParam(m_preview, e->param, m_param);
    }
    ImGui::SetNextItemWidth(-120);
    if (ImGui::SliderFloat("Distance", &m_distance, 0.0f, 100.0f, "%.1f m")) m_mixer.setPosition(m_preview, {0, 0, m_distance});

    const Stats& st = m_mixer.stats();
    ImGui::SeparatorText("Voices");
    ImGui::TextDisabled("%u mixed, %u virtual, %.3f ms", st.realVoices, st.virtualVoices, st.updateMs);
    std::vector<VoiceInfo> voices;
    m_mixer.voices(voices);
    for (const VoiceInfo& v : voices)
        ImGui::Text("%s%s  %.2f  %.1f m", v.event.c_str(), v.virtualVoice ? " (virtual)" : v.loop ? " (loop)" : "", v.audibility, v.distance);
    ImGui::SeparatorText("Culled (newest first)");
    const auto& culls = m_mixer.culls();
    int shown = 0;
    for (auto it = culls.rbegin(); it != culls.rend() && shown < 12; ++it, ++shown) ImGui::TextDisabled("%s  %s", it->event.c_str(), it->reason);
    ImGui::End();
}

// ---------------------------------------------------------------- 라이브 연결

void App::refreshLive(bool force) {
    if (m_project.empty()) return;
    if (force || m_time >= m_liveCheck) {
        m_liveCheck = m_time + 1.0;
        std::vector<Instance> found = findInstances(m_project);
        std::vector<bool> on(found.size(), true);
        for (size_t i = 0; i < found.size(); ++i)
            for (size_t j = 0; j < m_live.size(); ++j)
                if (m_live[j].pid == found[i].pid && j < m_liveOn.size()) on[i] = m_liveOn[j];
        m_live = std::move(found);
        m_liveOn = std::move(on);
    }
    if (m_time < m_stateCheck) return;
    m_stateCheck = m_time + 0.5;
    m_liveState.clear();
    for (size_t i = 0; i < m_live.size(); ++i) {
        if (!m_liveOn[i]) continue;
        const HttpResult r = request(m_live[i], "GET", "audio/state", {}, 200);
        if (r.status == 200) {
            m_liveState = r.body;
            m_liveJson = {};
            if (!Json::parse(m_liveState, m_liveJson)) m_liveState.clear();
        }
        break; // 상태는 처음 고른 하나만 본다
    }
}

void App::playInGame(const EventDef& e) {
    std::string body = "event=" + urlEncode(e.name);
    if (!e.param.empty()) body += "&p." + urlEncode(e.param) + "=" + std::to_string(m_param);
    int sent = 0;
    for (size_t i = 0; i < m_live.size(); ++i) {
        if (!m_liveOn[i]) continue;
        if (request(m_live[i], "POST", "audio/play", body).status == 200) ++sent;
    }
    m_liveError = sent ? std::string() : "No running game answered (is it a development build of this project?)";
}

void App::reloadInGames() {
    for (size_t i = 0; i < m_live.size(); ++i)
        if (m_liveOn[i]) request(m_live[i], "POST", "audio/reload");
}

void App::drawLive() {
    ImGui::Begin("Live");
    if (m_live.empty()) {
        ImGui::TextDisabled("No running game or editor for this project.");
        ImGui::TextWrapped("Run a development build of the game (or the MealKit editor) for this project: it appears here. Saving updates it at once.");
        ImGui::End();
        return;
    }
    for (size_t i = 0; i < m_live.size(); ++i) {
        const Instance& in = m_live[i];
        bool on = m_liveOn[i];
        const std::string label = in.kind + " (pid " + std::to_string(in.pid) + ", engine " + in.engine + ")";
        if (ImGui::Checkbox(label.c_str(), &on)) m_liveOn[i] = on;
        if (in.protocol != kLiveProtocol) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "protocol %u (tool %u)", in.protocol, kLiveProtocol);
        }
    }
    const EventDef* e = (m_selected >= 0 && m_selected < static_cast<int>(m_file.events.size())) ? &m_file.events[static_cast<size_t>(m_selected)] : nullptr;
    const bool dirty = writeEvents(m_file) != m_savedText;
    ImGui::BeginDisabled(!e);
    if (ImGui::Button("Play in game") && e) {
        if (dirty) save(); // 게임은 저장된 파일을 듣는다
        playInGame(*e);
    }
    ImGui::EndDisabled();
    if (dirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("(saves first)");
    }
    if (!m_liveError.empty()) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", m_liveError.c_str());

    if (!m_liveState.empty()) {
        const Json& j = m_liveJson;
        ImGui::SeparatorText("Game audio");
        ImGui::TextDisabled("%d event(s), %d mixed, %d virtual", static_cast<int>(j["events"].num()), static_cast<int>(j["stats"]["realVoices"].num()),
                            static_cast<int>(j["stats"]["virtualVoices"].num()));
        for (const Json& v : j["voices"].array)
            ImGui::Text("%s%s  %.2f  %.1f m", v["event"].str().c_str(), v["virtual"].flag() ? " (virtual)" : "", v["audibility"].num(), v["distance"].num());
        ImGui::SeparatorText("Game buses");
        for (const Json& b : j["buses"].array) {
            char overlay[48];
            std::snprintf(overlay, sizeof(overlay), "%s %.2f", b["name"].str().c_str(), b["peak"].num());
            ImGui::ProgressBar(static_cast<float>(std::min(b["peak"].num(), 1.0)), ImVec2(-FLT_MIN, 0), overlay);
        }
    }
    ImGui::End();
}

void App::drawQuitDialog() {
    if (!m_quitAsked) return;
    const bool dirty = !m_project.empty() && writeEvents(m_file) != m_savedText;
    if (!dirty) {
        m_quitConfirmed = true;
        return;
    }
    ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Save the changes to assets/sounds.events?");
        if (ImGui::Button("Save")) {
            if (save()) m_quitConfirmed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't save")) m_quitConfirmed = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_quitAsked = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace mkaudio::tool
