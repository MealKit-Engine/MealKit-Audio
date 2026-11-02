// MealKit Audio executable: SDL3 window + SDL_Renderer + Dear ImGui (docking). The actual screen is App.
//   MealKitAudio [--project <MealKit project folder>] [--select <event>] [--frames N --capture <png>]
//   MealKitAudio --golden --project <folder> [--update] [--event <name>]   golden render tests of the project's events without a window
//     (<project>/tests/audio/<event>.golden, mkaudio/Golden.h): "GOLDEN ok|new|updated|FAIL <event> ...", exit code 1 on a failure
#include "App.h"
#include "mkaudio/Golden.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::path(std::u8string(utf8.begin(), utf8.end())); }
std::string utf8Of(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Golden render tests: a missing golden file is written (new), --update rewrites all, otherwise each render is compared with its file
int runGolden(const std::string& project, bool update, const std::string& only) {
    const std::filesystem::path assets = pathOf(project) / "assets";
    mkaudio::EventFile file;
    std::vector<mkaudio::ParseMessage> messages;
    if (!mkaudio::loadEventFile(utf8Of(assets / "sounds.events"), file, &messages)) {
        std::printf("GOLDEN FAIL cannot read %s\n", utf8Of(assets / "sounds.events").c_str());
        return 1;
    }
    mkaudio::resolveClips(file, [&](const std::string& name) {
        std::error_code ec;
        const std::filesystem::path p = assets / pathOf(name);
        return std::filesystem::is_regular_file(p, ec) ? utf8Of(p) : std::string();
    });
    const std::filesystem::path dir = pathOf(project) / "tests" / "audio";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    int failed = 0, count = 0;
    for (const mkaudio::EventDef& e : file.events) {
        if (!only.empty() && e.name != only) continue;
        ++count;
        mkaudio::Envelope actual;
        std::string error;
        if (!mkaudio::renderEnvelope(file, e.name, actual, error)) {
            std::printf("GOLDEN FAIL %s: %s\n", e.name.c_str(), error.c_str());
            ++failed;
            continue;
        }
        const std::filesystem::path golden = dir / (e.name + ".golden");
        std::ifstream in(golden, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const bool exists = static_cast<bool>(in);
        in.close();
        if (update || !exists) {
            std::ofstream(golden, std::ios::binary) << mkaudio::writeEnvelope(actual);
            std::printf("GOLDEN %s %s (%.2f s)\n", exists ? "updated" : "new", e.name.c_str(),
                        static_cast<double>(actual.rms.size()) * actual.blockFrames / actual.sampleRate);
            continue;
        }
        mkaudio::Envelope expected;
        if (!mkaudio::parseEnvelope(text.str(), expected, error)) {
            std::printf("GOLDEN FAIL %s: %s: %s\n", e.name.c_str(), utf8Of(golden).c_str(), error.c_str());
            ++failed;
            continue;
        }
        const mkaudio::GoldenDiff diff = mkaudio::compareEnvelopes(expected, actual);
        if (diff.same) std::printf("GOLDEN ok %s\n", e.name.c_str());
        else {
            std::printf("GOLDEN FAIL %s: %s (rewrite with --update if intended)\n", e.name.c_str(), diff.reason.c_str());
            ++failed;
        }
    }
    if (!only.empty() && count == 0) {
        std::printf("GOLDEN FAIL no event named %s\n", only.c_str());
        ++failed;
    }
    std::printf("GOLDEN %s: %d event(s), %d failed\n", failed ? "FAIL" : "PASS", count, failed);
    std::fflush(stdout);
    return failed ? 1 : 0;
}

// A nice-looking system font (ImGui's default vector font if none)
void loadFont(float size) {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
#endif
    };
    for (const char* path : candidates)
        if (SDL_GetPathInfo(path, nullptr) && io.Fonts->AddFontFromFileTTF(path, size)) return;
    ImFontConfig config;
    config.SizePixels = size;
    io.Fonts->AddFontDefaultVector(&config);
}

} // namespace

int main(int argc, char** argv) {
    std::string project, capture, select, onlyEvent;
    bool golden = false, update = false;
    long frames = 0; // --frames N: quit after N frames (--capture saves the last screen as PNG, for docs/tests)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = std::strtol(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) capture = argv[++i];
        else if (std::strcmp(argv[i], "--select") == 0 && i + 1 < argc) select = argv[++i];
        else if (std::strcmp(argv[i], "--golden") == 0) golden = true;
        else if (std::strcmp(argv[i], "--update") == 0) update = true;
        else if (std::strcmp(argv[i], "--event") == 0 && i + 1 < argc) onlyEvent = argv[++i];
        else if (std::strcmp(argv[i], "--version") == 0) {
            SDL_Log("MealKit Audio %s", MKAUDIO_VERSION);
            return 0;
        }
    }

    if (golden) {
        if (project.empty()) {
            std::printf("--golden needs --project <folder>\n");
            return 1;
        }
        return runGolden(project, update, onlyEvent);
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    SDL_Window* window = SDL_CreateWindow("MealKit Audio", static_cast<int>(1400 * scale), static_cast<int>(860 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_Log("Cannot create the window: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    // Window layout goes in the user folder (not left in the project)
    static std::string ini;
    if (char* pref = SDL_GetPrefPath("MealKit", "MealKit Audio")) {
        ini = std::string(pref) + "imgui.ini";
        SDL_free(pref);
        io.IniFilename = ini.c_str();
    }
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.FrameRounding = 3.0f;
    loadFont(16.0f);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    mkaudio::tool::App app;
    app.window = window;
    app.init(project);
    if (!select.empty()) app.select(select);

    Uint64 last = SDL_GetPerformanceCounter();
    std::string shownTitle;
    long frame = 0;
    for (bool running = true; running;) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) app.requestQuit();
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) app.requestQuit();
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        const Uint64 now = SDL_GetPerformanceCounter();
        const double dt = static_cast<double>(now - last) / static_cast<double>(SDL_GetPerformanceFrequency());
        last = now;

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame(dt);
        ImGui::Render();
        if (app.canQuit()) running = false;

        const std::string title = app.title();
        if (title != shownTitle) {
            SDL_SetWindowTitle(window, title.c_str());
            shownTitle = title;
        }
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, 0.08f, 0.08f, 0.09f, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (frames > 0 && ++frame >= frames) {
            if (!capture.empty()) {
                SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
                if (!shot || !SDL_SavePNG(shot, capture.c_str())) SDL_Log("Cannot save %s: %s", capture.c_str(), SDL_GetError());
                SDL_DestroySurface(shot);
            }
            running = false;
        }
        SDL_RenderPresent(renderer);
    }

    app.shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
