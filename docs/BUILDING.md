# Building PS5CEMU-HAR

`make release` builds everything from source on Linux and writes the app to `build/app/PPSA99360`
and `dist/PS5CEMU-HAR-vVERSION.zip`. Ubuntu 24.04 is what it is built on; WSL works.

## Requirements

- `clang-18`, `lld-18` and the LLVM 18 tools
- `cmake`, `ninja`, `git`, `make` and `python3`
- For RADV, the Vulkan driver: `meson`, Python's `mako` and `packaging`, `rsync`,
  `glslangValidator`, and LLVM, Clang, libclc, SPIRV-Tools and the SPIR-V translator for Mesa's
  OpenCL kernels

On Ubuntu 24.04:

```bash
pip install meson mako packaging
sudo apt install rsync flex glslang-tools llvm-18-dev libclang-18-dev libclc-18-dev libllvmspirvlib-18-dev llvm-spirv-18 spirv-tools
```

## Targets

```bash
make radv      # RADV, the Vulkan driver, with the driver patches RADV_PATCHES names (0006 by default)
make azahar    # Azahar's core and its PS5 frontend (build/azahar)
make melonds   # melonDS's core and its PS5 frontend (build/melonds)
make release   # the app (build/app/PPSA99360), dist/PS5CEMU-HAR-vVERSION.zip with its SHA256SUMS,
               # and dist/ps5cemu-vVERSION.elf, the eboot's ELF with its symbols
make check     # the same build with a stand-in for RADV: checks everything else, not an app
```

**The version** is set in one place, `VERSION` in the `Makefile`. The launcher and the boot log
show it (`port/CMakeLists.txt` reads it), and `tools/package.sh` writes it into the app's
`param.json`: `3.0.0` is `contentVersion` `03.000.000` and `masterVersion` `03.00` (`2.0.0d` was
`02.000.004`: the patch number in hundreds, then the letter).

**Crash reports.** A crash leaves `[crash]` lines in the boot log: the crashed instruction, the
registers and the code addresses on the crashed thread's stack, as numbers. With the release's ELF
(`make release` keeps it in `dist/`), `tools/symbolize-crash.py` turns them into function names:

```bash
python3 tools/symbolize-crash.py dist/ps5cemu-v3.0.0.elf boot.log
```

`make help` lists every target. `make deps` fetches every input at the revision pinned in
`tools/deps.json`, then builds the libraries the emulators need for the PS5 into `build/sysroot`:

- Cemu, and Mihawk's PS5 build of Azahar (with its dynarmic and the submodules it builds with)
- melonDS, at its 1.1 release's commit
- the PS5 Native App Boilerplate (payload SDK, runtime, packaging tool)
- pacbrew's prebuilt PS5 libraries
- Boost, pugixml, libzip, glslang and RapidJSON
- Mihawk's PS5_Mesa, PS5_Vulkan and payload SDK fork
- compiler-rt's emulated TLS and CPU-model builtins
- the Cemu community graphic packs
- ProsperoEden

**RADV.** `make radv` builds the driver with PS5_Vulkan's own recipe, from the pinned Mesa fork and
the payload SDK fork, whose platform layer the PS5 winsys is built on. It then builds the same
revision again with the port's driver patches (`patches/mesa`) that `RADV_PATCHES` names, configured
as the recipe configures its release, and that build is the one the app links:

- `RADV_PATCHES=0006`, the default: VideoOut's refresh rate asked for again once the new launcher is
  gone. Without it, a game with **120 Hz output** stays at the launcher's 59.94 Hz, because the
  driver opens VideoOut once per process and the new launcher opens it first.
- `RADV_PATCHES=0004,0006`: those patches, by number; `RADV_PATCHES=all`: every patch, for
  [DRIVER-PERFORMANCE.md](DRIVER-PERFORMANCE.md)'s A/B runs; `RADV_PATCHES=` (empty): the recipe's
  archive as it is.

The patched tree is made in a temporary index of the fork's repository, so its checkout stays at
the pin, and a change of patches recompiles only the files they touch. `make build` links again
whenever the driver archive changed, which ninja alone would not notice. A RADV build made elsewhere
can be used instead: point `RADV_ARCHIVE` and `RADV_SDK` at it and at the SDK fork it was built
with. The link follows PS5_Vulkan's recipe for titles (`tools/link.sh`).

**Azahar.** `make azahar` (`tools/build-azahar.sh`) builds Azahar's core libraries with the same
PS5 toolchain as Cemu (`tools/ps5.cmake`), without its own frontends, and builds the app's PS5
frontend for it (`port/azahar`'s window, input, sound and in-game menu hook) in Azahar's tree. It
shares Cemu's copies of fmt, glslang, zstd and OpenSSL, so the app has one of each, and writes the
list of archives the app links to `build/azahar/azahar_ps5_libraries.txt`. Without that build, the
app still builds, and 3DS games say why they cannot start.

**melonDS.** `make melonds` (`tools/build-melonds.sh`) builds melonDS's core (its JIT, its software
renderer, teakra, FreeBIOS) with the same toolchain, with `patches/melonds` on a `ps5` branch of
`.deps/melonDS` (`tools/melonds-patches.sh`): the build takes an outside frontend, and the JIT's code
goes in executable direct memory (`ps5platform/exec.h`) with no fast memory. Its PS5 frontend
(`port/melonds`: the DS's platform, the DualSense, AudioOut) is built in melonDS's tree, and
`build/melonds/melonds_ps5_libraries.txt` lists the archives the app links. teakra and xxHash are
renamed (`Teakra`, `XXH_NAMESPACE`) so they cannot meet Azahar's. The screens' shaders
(`port/melonds/shaders/`) are compiled into `port/melonds/shaders.h` by `tools/render-shaders.sh`,
as the UI kit's are. Without that build, the app still builds, and DS games say why they cannot
start.

**Link check.** `make check` links with a driver stand-in instead of RADV. That proves everything
else compiles, links and packages, but its output (`build/app-check`) is not an app.

## How it fits together

| Path | What it is |
|---|---|
| `port/ps5/` | The console layer: the DualSense, VideoOut through Vulkan, logging, notifications, the sandbox escape, JIT memory and thread placement |
| `port/cemu/` | Cemu's platform classes for the PS5: the memory mapper, fibers, AudioOut, the DualSense controller, and a Microsoft-ABI bridge for the recompiler, since the PS5 target has no `ms_abi` |
| `port/app/` | Cemu's start-up without wxWidgets, the game list, game icons and box art, graphic packs, controller settings, installs, and what the app shows over a game: both in-game menus, the GamePad's screen and the touchpad's cursor |
| `port/azahar/` | Azahar's side: the 3DS library (read by the launcher itself), its controls, and its PS5 frontend (built in Azahar's tree): the window on VideoOut, the DualSense as the 3DS, AudioOut, CIA installs |
| `port/melonds/` | The DS side: its library (read by the launcher itself), and melonDS's PS5 frontend (built in melonDS's tree): the DS's platform, its two screens on VideoOut, the DualSense as the DS, AudioOut |
| `port/frontend/` | The launcher (`shell/`, [docs/UI-REDESIGN.md](UI-REDESIGN.md)): the Wii U, 3DS and DS sides of one shell, opening on the side last used, drawn with the UI kit; the settings (`settings.h`), games' own settings included |
| `port/ui/` | The UI kit the new launcher draws with: Vulkan on the RADV the app links, presenting to VideoOut; signed-distance shapes and text (Lexend baked by `tools/render-sdf-font.sh`), pictures, springs, the DualSense as actions, and the design's tokens |
| `port/main_ps5.cpp` | The entry point: the sandbox escape, logs, Cemu's core, the launcher, the game |
| `patches/cemu/`, `patches/azahar/`, `patches/melonds/` | The port's changes to Cemu's, Azahar's and melonDS's own files |
| `tools/` | The dependencies, the builds, the PS5 link (`link.sh`), the packaging (`package.sh`), the artwork and the launcher's preview |
| `sce_sys/` | The title's `param.json` and its home screen background, `pic0.dds` |

### Changing Cemu, Azahar or melonDS

The port's changes to each emulator are the patches in `patches/cemu/`, `patches/azahar/` and
`patches/melonds/`, which the build puts on a branch named `ps5` in `.deps/Cemu`, `.deps/PS5_Azahar`
and `.deps/melonDS` (`tools/cemu-patches.sh apply`, `tools/azahar-patches.sh apply`,
`tools/melonds-patches.sh apply`). To change one, edit its checkout
on that branch, commit there, then run its script with `export` to write the patches again.

## Artwork

Everything the app and this repository show is drawn by a script, so it can be changed in one place
and drawn again:

| Script | What it draws |
|---|---|
| `tools/render-sdf-font.sh` | Lexend's signed-distance atlas for the launcher (`port/ui/fonts/lexend.sdf`, committed) |
| `tools/render-menu-fonts.py` | Lexend's Regular, Medium and SemiBold for the in-game menus, which ImGui draws (`port/ui/fonts/Lexend-*.ttf`, committed). Needs fontTools |
| `tools/render-gametdb.py` | GameTDB's game information, as the game pages and in-game menus read it (`port/app/gametdb`, committed) |
| `tools/render-background.py` | The Wii U Homebrew Launcher's background as a still picture, which the icons, the home screen background and the banner draw on. The launcher draws it moving (`port/frontend/shell/shell.cpp`) |
| `tools/render-icons.py` | The launcher's icons: the GamePad on the bubbles, the 3DS on the waves, the DS on melon green with its pixels, and the Wii U and 3DS side by side. Run by `package.sh` |
| `tools/render-shaders.sh` | The UI kit's and the DS screens' SPIR-V (`port/ui/shaders.h`, `port/melonds/shaders.h`, committed), with a glslangValidator |
| `tools/render-presentation.py` | The PS5 home screen's art: the background (`sce_sys/pic0.dds`, installed as `pic0.dds` and `pic1.dds`, a 3840x2160 BC7 DDS it encodes itself) and the tile (`sce_sys/icon0.png`), both the README banner's design. Needs Pillow, numpy and the banner's fonts; its output is committed, so the build does not |
| `tools/render-banner.py` | This repository's banner, `docs/banner.svg` |

All but `render-presentation.py` and `render-menu-fonts.py` need only Python's standard library.

## The launcher on a PC

`tools/preview-shell.sh` builds the launcher's own code for the PC and saves its screens as PNGs in
`build/shell-preview`, so a change to the layout can be seen without a console: its own code and the
UI kit run as on the console, drawing with the PC's Vulkan (Mesa's lavapipe will do: `libvulkan-dev`
and `mesa-vulkan-drivers`, with `libfreetype-dev`), with sample games, graphic packs and controllers
in place of the emulators (`tools/launcher-preview/console.cpp`). A script presses the DualSense's
buttons and says when to save a screen. It needs `make deps` first, and zlib's headers
(`zlib1g-dev`). Its scripts are
`tools/launcher-preview/shell-screens.txt` (every screen), `shell-first.txt` (a first start, with
`PREVIEW_FIRST=1`) and `shell-ask.txt` (the side chooser, with `PREVIEW_ASK=1`).
