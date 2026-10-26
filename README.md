# MealKit Audio

Sound event runtime for games, part of the [MealKit](https://github.com/MealKit-Engine/MealKit) tool family. Free, MIT.

Game code plays an **event name** with parameters; how it sounds is data a sound designer edits:

```cpp
mkaudio::Mixer mixer;
mixer.init();
mixer.setEvents(events);                                   // std::vector<mkaudio::EventDef>
mixer.play("hit", {x, y, z}, {{"speed", 6.0f}});           // every frame: mixer.update(dt)
```

The runtime (`mkaudio::Mixer`, built on [miniaudio](https://github.com/mackron/miniaudio)) handles, FMOD/Wwise style:

- containers: random (never the same clip twice in a row), switch, blend (crossfade along a parameter)
- random volume and pitch, parameter curves for volume, pitch and low-pass
- 3D position with linear attenuation, buses with volume and peak meters
- instance limits with stealing (oldest, quietest, none), cooldowns, same-frame grouping
- voice management: a cap on mixed voices, inaudible loops become virtual and resume in place
- subtitles for sounds that carry a caption key

It does not read files of its own format yet: the program that uses it turns its data into `EventDef` values (MealKit reads `assets/sounds.table`). A visual authoring tool for sound designers comes next.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # mixes without an audio device
```

C++20. To use it from another CMake project, `add_subdirectory` this folder and link `mkaudio`; set `MKAUDIO_MINIAUDIO_DIR` and `MKAUDIO_STB_DIR` if your project already has miniaudio and stb_vorbis, otherwise the copies in `third_party/` are used.
The library includes no engine headers, so it can be used outside MealKit. MealKit vendors a pinned copy in its `third_party/mkaudio/`.

## Third-party code

| Library | Version | License | Use |
|---|---|---|---|
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | MIT-0 | devices, node graph, decoding, 3D |
| [stb_vorbis](https://github.com/nothings/stb) | v1.22 (stb 2c980bb) | MIT / public domain | Ogg Vorbis streaming |

## License

MIT, see `LICENSE`.
