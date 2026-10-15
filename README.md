<p align="center">
  <img src="docs/banner.svg" alt="PS5CEMU-HAR: Cemu, Azahar and melonDS, the Wii U, Nintendo 3DS and Nintendo DS emulators, on PlayStation 5 homebrew" width="100%">
</p>

<p align="center">
  <a href="https://github.com/premohq/PS5CEMU-HAR/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/premohq/PS5CEMU-HAR?label=release&color=3b8cff"></a>
  <img alt="Platform: PS5 homebrew" src="https://img.shields.io/badge/platform-PS5%20homebrew-1f5fd6">
  <img alt="Emulation: Cemu, Azahar and melonDS" src="https://img.shields.io/badge/emulation-Cemu%20%C2%B7%20Azahar%20%C2%B7%20melonDS-5cd3ff">
  <img alt="Languages: 31" src="https://img.shields.io/badge/languages-31-62c454">
  <a href="LICENSE"><img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-a9a6ff"></a>
</p>

<p align="center">
  <b>PS5CEMU-HAR</b> plays your Wii U, Nintendo 3DS and Nintendo DS games on a jailbroken PS5,<br>
  with <a href="https://github.com/cemu-project/Cemu">Cemu</a>, <a href="https://github.com/azahar-emu/azahar">Azahar</a>
  and <a href="https://github.com/melonDS-emu/melonDS">melonDS</a> in one launcher made for the TV and the DualSense.
</p>

<p align="center">
  <a href="https://github.com/premohq/PS5CEMU-HAR/raw/releases/PS5CEMU-HAR-v4.0.1.zip"><b>Download 4.0.1</b></a> (ZIP, 52.5 MB) ·
  <a href="https://github.com/premohq/PS5CEMU-HAR/releases/tag/v4.0.1">Release notes</a> ·
  <a href="#install">Install</a> ·
  <a href="docs/GUIDE.md">Guide</a> ·
  <a href="docs/COMPATIBILITY.md">Compatibility</a> ·
  <a href="#credits">Credits</a>
</p>

---

## What's new in 4.0.1

Fixes for 4.0.0. The details are in the [release notes](https://github.com/premohq/PS5CEMU-HAR/releases/tag/v4.0.1).

- **Network shares work again.** Connecting to an SMB share no longer crashes the app
  ([#46](https://github.com/premohq/PS5CEMU-HAR/issues/46), [#49](https://github.com/premohq/PS5CEMU-HAR/issues/49)),
  and a share's tab opens in the folder chooser ([#49](https://github.com/premohq/PS5CEMU-HAR/issues/49)).
- **DS sound at the right speed:** no more crackling or too-fast music, and the speed shown is the DS's
  ([#47](https://github.com/premohq/PS5CEMU-HAR/issues/47)).
- **Back to the library after a game** on more setups: the app tries each place it can start from, and a
  home screen tile's game says *Quit to the home screen* ([#48](https://github.com/premohq/PS5CEMU-HAR/issues/48)).
- **Artic Base and Artic Setup** ([#45](https://github.com/premohq/PS5CEMU-HAR/issues/45)): connections time
  out instead of hanging, a 3DS that closes the connection no longer closes the app, the app starts over
  cleanly after a failed load, and the errors say what went wrong. Artic Setup says which keys
  `aes_keys.txt` lacks before it starts, and *Copy logs to USB* includes Azahar's log of the session before.

## What's new in 4.0.0

The biggest update yet. The full list is in the [release notes](https://github.com/premohq/PS5CEMU-HAR/releases/tag/v4.0.0).

- **31 languages: every one the PS5 has.** The launcher, Settings and the in-game menus are translated
  into 30 new languages, with your games' names in your language too:
  English (UK and US), German, French (France and Canada), Spanish (Spain and Latin America), Italian,
  Dutch, Portuguese (Portugal and Brazil), Russian, Ukrainian, Polish, Czech, Hungarian, Romanian,
  Greek, Turkish, Finnish, Swedish, Danish, Norwegian, Indonesian, Japanese, Korean, Chinese
  (Simplified and Traditional), Thai, Vietnamese and Arabic.
- **Nintendo DS games** on melonDS, a third system beside the Wii U and the 3DS.
- **Your games on the PS5 home screen:** *Add to home screen* gives any game a tile that starts it
  straight away, and *Remove from home screen* takes it off again.
- **Wii U mods** from a USB drive, with a [step-by-step guide](docs/MODS.md).
- **Eight themes:** Link Jack, Classic (the original 2.0 launcher, back), Eden, Meyer Oscar and Meyer
  Oscar Light, Midnight, Phantom and Terminal, or your own.
- **Play from your PC or NAS** over the network (SMB or NFS).
- **RetroAchievements** for DS games.
- **Faster Wii U games:** Breath of the Wild is back near 60 fps at 1440p, and holds 60 more often at
  4K.
- **Pokémon X, Y, Omega Ruby, Alpha Sapphire, Sun, Moon, Ultra Sun and Ultra Moon** drawn right.
- **And more:** your phone's browser as the GamePad's or a handheld's touch screen, Wii U accounts
  and save backups to USB, updates and DLC found on your drives and installed with one press, AMD FSR
  1, settings for each game, buttons that swap, a turbo button, rumble that works, Skylanders Giants,
  your own launcher music, and a quicker start.

## Features

| | Wii U | Nintendo 3DS | Nintendo DS |
|---|---|---|---|
| **Emulator** | Cemu, x64 recompiler, Vulkan | Azahar, dynarmic recompiler, Vulkan | melonDS, ARM recompiler |
| **Plays** | `.wua`, `.wud`, `.wux`, unpacked games, ZIP archives | `.3ds`, `.cci`, `.cxi`, `.app`, `.3dsx`, `.elf`, compressed `.z*`, installed `.cia`, ZIP archives | `.nds`, `.srl`, ZIP archives |
| **Picture** | 4K output, 120 Hz, FSR 1, graphic packs | up to 10x resolution, texture filters, layouts, borders | sharp, smooth or square pixels, layouts, borders |
| **Extras** | mods, accounts, amiibo, Skylanders portals, updates and DLC | save states, cheats, amiibo, mods, Artic Base | save states, cheats, RetroAchievements |

- **One launcher for all three:** box art and game info from GameTDB, search, sorting and favourites,
  a page for each game, settings for each game, and an in-game menu on every system.
- **Made for the DualSense:** every button remappable, motion controls, rumble, the touchpad as the
  touch screen, and up to four players on the Wii U.
- **Updates itself:** a new release installs from inside the app.

### ZIP games (all three systems)

Place a `.zip` containing a supported ROM in the relevant Wii U, 3DS or DS games folder. Nested folders and multiple ROMs per ZIP are supported. A Wii U ZIP of an unpacked game must contain `code/*.rpx` and `meta/meta.xml` (and normally `content/`); it appears as one title. The launcher reads only the ZIP directory when scanning; on first launch, the selected ROM (or the whole unpacked Wii U title) is validated and extracted to `/data/ps5cemu/cache/zip-games/`. Further launches reuse the cache until the ZIP size or timestamp changes. This requires enough free PS5 storage for the uncompressed game. Clear that cache folder to recover storage. ZIPs are **not** streamed or run in place. Encrypted/password-protected ZIPs are unsupported. Compression does not bypass game decryption or BIOS/key requirements; DSi and CIA installation remain subject to the emulators' existing restrictions. ZIP entries use archive/entry filenames as library titles until the game is opened.

## Install

1. **A PS5 that runs homebrew.** Any firmware up to 13.60, with etaHEN, OnionHEN, or an ELF loader on
   port 9021. See the [firmware and setup guide](docs/HEN-SETUP.md).
2. **Copy the app.** Download the [latest ZIP](https://github.com/premohq/PS5CEMU-HAR/raw/releases/PS5CEMU-HAR-v4.0.1.zip),
   extract it and copy the `PPSA99360` folder to `/data/homebrew/PPSA99360`.
3. **Let your HEN in.** etaHEN: add `PPSA99360` to its app jailbreak list. OnionHEN: add `PPSA99360`
   at the end of `exact_title_ids` in `config.ini`, with no comma after it. Any other: have an ELF
   loader on port 9021.
4. **Add your games** (below), and start PS5CEMU-HAR from the home screen. The Setup check shows
   what's ready and what to do about the rest.

Updating: the app offers new releases itself. Everything in `/data/ps5cemu` is kept.

## Your games

| | Games folder | Also needs |
|---|---|---|
| **Wii U** | `/data/ps5cemu/games` | disc keys in `/data/ps5cemu/keys.txt` for encrypted `.wud` and `.wux` |
| **3DS** | `/data/ps5cemu/azahar/games` | decrypted dumps, or your console's `aes_keys.txt` in `/data/ps5cemu/azahar/sysdata`; `.cia` files install from Settings |
| **DS** | `/data/ps5cemu/melonds/games` | nothing: your own BIOS and firmware in `/data/ps5cemu/melonds/bios` are optional |

Any other folder, a USB drive or a network share works too: **Settings > Game files** on each system.
No games, keys or firmware are included. Use dumps of games and hardware you own.

## Controls

| | |
|---|---|
| **L1 / R1** | Library and Settings |
| **Touchpad click** | The next system |
| **Cross / Circle** | Play or open / back |
| **Options** | A game's menu: game settings, saves, graphic packs, mods, home screen tile |
| **Touchpad click + Options** | The in-game menu |
| **Touchpad** | The touch screen (GamePad, 3DS and DS) |

Every shortcut, the in-game menu and the rest are in the [guide](docs/GUIDE.md).

## Documentation

- [Guide](docs/GUIDE.md): the launcher, each system, controls, saves, mods, the remote touch screen and where files are kept
- [Firmware and setup](docs/HEN-SETUP.md) · [Mods](docs/MODS.md) · [Compatibility](docs/COMPATIBILITY.md)
- [Building from source](docs/BUILDING.md) · [Driver performance](docs/DRIVER-PERFORMANCE.md)
- Problems: **Settings > Diagnostics > Copy logs to USB**, then [open an issue](https://github.com/premohq/PS5CEMU-HAR/issues/new/choose)

## Building

`make release` on Ubuntu 24.04 (WSL works) downloads every dependency at its pinned version, builds
the three emulators and the launcher, and writes the app to `build/app/PPSA99360` and a ZIP to
`dist/`. See [docs/BUILDING.md](docs/BUILDING.md).

## Credits

- [Cemu](https://github.com/cemu-project/Cemu) by the Cemu team and contributors (MPL-2.0)
- [Azahar](https://github.com/azahar-emu/azahar) by the Azahar team and the Citra contributors
  before them (GPL-2.0-or-later), with dynarmic by MerryMage and contributors
- [melonDS](https://github.com/melonDS-emu/melonDS) by Arisotura and the melonDS team (GPL-3.0),
  with its FreeBIOS replacements
- **Mihawk** for RADV on PS5 ([PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa),
  [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), the payload SDK fork and its platform layer)
  and the PS5 ports of Azahar and dynarmic ([PS5_Azahar](https://github.com/mihawk-99/PS5_Azahar)),
  with contributions from **mpereiraesaa**
- **BlackBearReloaded** for [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden), which
  the launcher's layout, artwork, Eden theme and software renderer build on, and the
  [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) for
  the app runtime, packaging and sandbox elevation
- **heydemoura** for the [PS5 Forwarder Format](https://github.com/heydemoura/ps5-forwarder-format)
  the home screen tiles use
- **John Törnblom** for the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), and
  **pacbrew** for their PS5 libraries
- [libsmb2](https://github.com/sahlberg/libsmb2) and [libnfs](https://github.com/sahlberg/libnfs) by
  Ronnie Sahlberg for the network shares, and [rcheevos](https://github.com/RetroAchievements/rcheevos)
  by RetroAchievements
- **dolphin78** for the remote touch screen's idea and prototype (#38), and **3lmerx503** for the
  rumble fix (#41)
- **Swordpdf** for the etaHEN jailbreak request from PS5SX2
- **Dimok** for the Wii U Homebrew Launcher's background, and the authors of the 3DS Homebrew
  Launcher, which inspired the 3DS side's
- The [Lexend](https://github.com/googlefonts/lexend) authors for the launcher's font (SIL OFL 1.1)
- [GameTDB](https://www.gametdb.com/) for box art and game information, and the authors of the
  [Cemu community graphic packs](https://github.com/cemu-project/cemu_graphic_packs)

## License

The app's own code is GPL-3.0-or-later (see [LICENSE](LICENSE)). Files derived from Cemu keep Cemu's
MPL-2.0, as their headers say; Azahar is GPL-2.0-or-later and melonDS GPL-3.0, and the app that
includes them is distributed under GPL-3.0-or-later. Other third-party components keep their own
licenses.

PS5CEMU-HAR is an independent homebrew project, not affiliated with or endorsed by Sony Interactive
Entertainment, Nintendo, or the Cemu, Azahar or melonDS projects. "PlayStation" and "PS5" are
trademarks of Sony Interactive Entertainment Inc.; "Wii U", "Nintendo 3DS" and "Nintendo DS" are
trademarks of Nintendo. No firmware, keys, games or SDK material is included. It is provided as is,
without warranty, for use with hardware and games you own.

## Support

PS5CEMU-HAR is free. If it brings your games back, you can support it at
[paypal.me/empthe](https://www.paypal.com/paypalme/empthe), or scan the code (the same one as in the
app's **Settings > About**):

<a href="https://www.paypal.com/paypalme/empthe"><img src="docs/support-qr.svg" width="120" alt="QR code for paypal.me/empthe"></a>
