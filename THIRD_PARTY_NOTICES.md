# Third-party notices

| Component | Where | Licence |
|---|---|---|
| [Ghostship](https://github.com/HarbourMasters/Ghostship) by HarbourMasters: the game's code (from the Super Mario 64 decompilation) and the port | downloaded at build time (`cmake/FetchGhostship.cmake`), compiled into the app; its own assets as `ghostship.o2r`, its extractor's descriptions as `config.yml` and `assets/` | MIT (`licenses/Ghostship-LICENSE.md` in the IPK) |
| [libultraship](https://github.com/Kenix3/libultraship) | part of Ghostship: graphics, audio, controllers, the settings menu | MIT (`licenses/libultraship-LICENSE`) |
| [Torch](https://github.com/HarbourMasters/Torch) | part of Ghostship: makes `sm64.o2r` from the ROM on the TV | MIT (`licenses/Torch-LICENSE`) |
| [Dear ImGui](https://github.com/ocornut/imgui), [cimgui](https://github.com/cimgui/cimgui) | Ghostship's settings menu | MIT |
| [StormLib](https://github.com/ladislav-zezula/StormLib) | `.otr` (MPQ) mods | MIT |
| [libzip](https://github.com/nih-at/libzip) 1.11.4 | `.o2r` archives | BSD-3-Clause |
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 | settings | MIT |
| [tinyxml2](https://github.com/leethomason/tinyxml2) 11.0.0 | asset descriptions | zlib |
| [spdlog](https://github.com/gabime/spdlog) 1.16.0 (with fmt) | logs | MIT |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | the extractor's descriptions | MIT |
| [libgfxd](https://github.com/glankk/libgfxd), [dr_libs](https://github.com/mackron/dr_libs), [thread-pool](https://github.com/bshoshany/thread-pool), [prism-processor](https://github.com/KiritoDv/prism-processor), [stb](https://github.com/nothings/stb), [GSL](https://github.com/microsoft/GSL) | parts of libultraship and Torch | MIT, public domain or MIT (stb), MIT (GSL) |
| [SDL-webOS](https://github.com/webosbrew/SDL-webOS) 2.30.12 | bundled as `lib/libSDL2-2.0.so.0` | zlib |
| [SDL_GameControllerDB](https://github.com/gabomdq/SDL_GameControllerDB) | `gamecontrollerdb.txt`, controller mappings | zlib |
| [Khronos OpenGL ES headers](https://github.com/KhronosGroup/OpenGL-Registry) | `third_party/khronos/`, build only | Apache-2.0 / MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) 0.63.0 by yhirose | the transfer page's server (`third_party/cpp-httplib/`) | MIT (`licenses/cpp-httplib-LICENSE`) |
| [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper | the setup screen and the menu (`src/port/BitmapFont.cpp`) | Public domain |
| [Titan One](https://fonts.google.com/specimen/Titan+One) by Rodrigo Fuenzalida | the lettering of the logo and icons (`scripts/make-logo.py`; the font is not in the repository) | SIL Open Font License 1.1 |

zlib, OpenGL ES and the C library are the TV's own.

This project contains no Nintendo game data. The Super Mario 64 ROM, and the
`sm64.o2r` archive made from it, are never part of this repository or the IPK:
you supply your own ROM. Super Mario 64 and Mario are trademarks of Nintendo;
this project is not affiliated with or endorsed by Nintendo or HarbourMasters.
