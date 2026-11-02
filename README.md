# MealKit Audio

Sound event runtime for games, part of the [MealKit](https://github.com/MealKit-Engine/MealKit) tool family. Free, MIT.

Game code plays an **event name** with parameters; how it sounds is data a sound designer edits:

```cpp
mkaudio::Mixer mixer;
mixer.init();
mixer.setEvents(events);                                   // std::vector<mkaudio::EventDef>
mixer.play("hit", {x, y, z}, {{"speed", 6.0f}});           // every frame: mixer.update(dt)
mixer.playMusic("explore");                                // switches on the next bar
mixer.stinger("victory", mkaudio::Quantize::Beat);
```

The runtime (`mkaudio::Mixer`, built on [miniaudio](https://github.com/mackron/miniaudio)) handles, FMOD/Wwise style:

- containers: random (never the same clip twice in a row), switch, blend (crossfade along a parameter)
- random volume and pitch, parameter curves for volume, pitch and low-pass
- 3D position with linear attenuation, buses with volume and peak meters
- instance limits with stealing (oldest, quietest, none), cooldowns, same-frame grouping
- voice management: a cap on mixed voices, inaudible loops become virtual and resume in place
- subtitles for sounds that carry a caption key
- occlusion: the game reports how blocked each 3D voice is (`emitters()` → its own rays → `setOcclusion()`), the voice gets quieter and muffled
- reverb (Freeverb) on buses that ask for it, moving smoothly to new values (`setReverb`, e.g. from reverb zones)
- ducking (a bus goes down while another one sounds) and snapshots (sets of bus volume/low-pass, faded in and out: `setSnapshot("pause", true)`)
- interactive music: tracks with BPM and time signature switch on the next beat or bar, scheduled to the PCM frame, with a crossfade;
  stems that come in one by one along a parameter (`setGlobalParam("tension", x)`), stingers on the next beat/bar

Events and buses live in one text file that merges in git (`mkaudio/Events.h`, in a MealKit project `assets/sounds.events`):

```text
# MealKit audio events. Edit them in MealKit Audio or by hand
@format audio-events 2

mixbus ambience
parent music
volume 0.6

mixbus music
duckBy voice
duckVolume 0.35

mixbus sfx
reverb true

snapshot pause
fade 0.25
volume music 0.6
lowpass sfx 900

music explore
stems music/explore_bass.ogg music/explore_drums.ogg
bpm 96
stemParam tension

event hit
clips sounds/hit_1.wav sounds/hit_2.wav sounds/hit_3.wav
param speed
paramMax 15
volumeAtMin 0.2
```

`mkaudio::loadEventFile` reads it (keys left out keep their defaults) and `saveEventFile` writes it back the same way.

## MealKit Audio (the authoring tool)

`MealKitAudio` is the app sound designers use: a list of events, every setting of an event with curve and distance graphs, clips picked from the project's `assets/`, buses with parents, mix volumes, reverb, ducking and live meters, music tracks (stems, tempo, transitions; preview with the stem parameter and stingers), snapshots (preview on/off) and a reverb preview, preview through the same runtime the game uses (parameter, distance and occlusion sliders), undo, and a live connection to a running MealKit game or editor of the project (play the event in the game, see the game's voices and buses; saving updates the game at once).

```sh
build/bin/MealKitAudio --project <MealKit project folder>
```

The MealKit editor lists it in its **Tools** menu when the folder with `MealKitAudio` and `mealkit-tool.txt` is in `%LOCALAPPDATA%\MealKit\tools\` (or pass `--tool-path build/bin` to the editor). Built with SDL3 and Dear ImGui.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # mixes without an audio device
```

The library, the tests and the tool are built when this folder is the top-level project (`MKAUDIO_BUILD_TESTS`, `MKAUDIO_BUILD_TOOL`).

C++20. To use it from another CMake project, `add_subdirectory` this folder and link `mkaudio`; set `MKAUDIO_MINIAUDIO_DIR` and `MKAUDIO_STB_DIR` if your project already has miniaudio and stb_vorbis, otherwise the copies in `third_party/` are used.
The library includes no engine headers, so it can be used outside MealKit. MealKit vendors a pinned copy in its `third_party/mkaudio/`.

## Third-party code

| Library | Version | License | Use |
|---|---|---|---|
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25+1df46ae (CVE-2026-32837 fix) | MIT-0 | devices, node graph, decoding, 3D |
| [stb_vorbis](https://github.com/nothings/stb) | v1.22 (stb 2c980bb) | MIT / public domain | Ogg Vorbis streaming |
| [SDL](https://github.com/libsdl-org/SDL) | 3.4.18 | Zlib | tool only: window, input, drawing |
| [Dear ImGui](https://github.com/ocornut/imgui) | v1.92.9b-docking (b48d1af) | MIT | tool only: user interface |
| [mkcontract](https://github.com/MealKit-Engine/MealKit-Contract) | see `third_party/mkcontract/VERSION.txt` | MIT | tool only: MealKit tool contract (live connection), copied with that repo's `tools/vendor.ps1` |

## License

MIT, see `LICENSE`.
