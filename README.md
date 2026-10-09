<p align="center">
  <img src="docs/banner.svg" alt="PS5CEMU-HAR: Cemu, Azahar and melonDS, the Wii U, Nintendo 3DS and Nintendo DS emulators, on PlayStation 5 homebrew" width="100%">
</p>

<p align="center">
  <strong>Wii U, Nintendo 3DS and Nintendo DS emulation in one PlayStation 5 homebrew app</strong><br>
  <a href="https://github.com/premohq/PS5CEMU-HAR/raw/releases/PS5CEMU-HAR-v3.5.0.zip"><strong>Download PS5CEMU-HAR 3.5.0</strong></a> (ZIP, 47.0 MB) ·
  <a href="https://github.com/premohq/PS5CEMU-HAR/releases/tag/v3.5.0">Release notes</a> ·
  <a href="#whats-new-in-350">What's new</a><br>
  <a href="#install">Install</a> · <a href="#wii-u-cemu">Wii U</a> · <a href="#nintendo-3ds-azahar">3DS</a> ·
  <a href="#nintendo-ds-melonds">DS</a> ·
  <a href="#controls">Controls</a> · <a href="docs/COMPATIBILITY.md">Compatibility</a> ·
  <a href="docs/BUILDING.md">Building</a> · <a href="#credits">Credits</a>
</p>

**PS5CEMU-HAR** is a homebrew app for jailbroken PS5 consoles. It bundles three emulators, played
with the DualSense, the first two rendering with Vulkan through Mihawk's PS5 port of the RADV driver:

- [Cemu](https://github.com/cemu-project/Cemu) for Wii U games
- [Azahar](https://github.com/azahar-emu/azahar) for Nintendo 3DS games
- [melonDS](https://github.com/melonDS-emu/melonDS) for Nintendo DS games (in the 3.5.1 beta)

One launcher holds them all: it opens on the Library of the side you used last, Wii U (Cemu), 3DS
(Azahar) or DS (melonDS), and a touchpad click goes to the next side. Each side has its own game
library, settings and in-game menu, and each game can have settings of its own.

This is an unofficial project, not affiliated with or endorsed by the Cemu, Azahar or melonDS teams,
Nintendo or Sony. All credit for the emulators goes to their developers.

> [!NOTE]
> **Status:** most Wii U games tried so far are playable, with working video, controls, sound and
> saves. 3DS support is newer and has had less testing on real hardware. If something goes wrong,
> please [report it with your logs](#reporting-problems).

## Install

> [!NOTE]
> **First, can your PS5 run homebrew?** That depends on your firmware, not on this app: a jailbroken
> console on a firmware the scene has an exploit for (as of late 2026, every firmware up to
> **13.60**, disc or digital, with **14.00+** not yet cracked). Once your console loads **etaHEN,
> OnionHEN, or just an ELF loader on port 9021**, PS5CEMU-HAR runs. The
> [firmware and setup guide](docs/HEN-SETUP.md) has the full picture, per firmware and per HEN.

1. Download [PS5CEMU-HAR-v3.5.0.zip](https://github.com/premohq/PS5CEMU-HAR/raw/releases/PS5CEMU-HAR-v3.5.0.zip)
   ([release notes](https://github.com/premohq/PS5CEMU-HAR/releases/tag/v3.5.0), [all releases](https://github.com/premohq/PS5CEMU-HAR/releases)) and extract it, or
   [build it yourself](docs/BUILDING.md).
2. Copy the `PPSA99360` folder to `/data/homebrew/PPSA99360` on your PS5.
3. Load your HEN and let it jailbreak `PPSA99360`:
   - **etaHEN:** add `PPSA99360` to its app jailbreak list.
   - **OnionHEN:** add `PPSA99360` to the end of `exact_title_ids` in `config.ini`, with **no comma
     after it** (a trailing comma makes OnionHEN ignore the whole list), then reload OnionHEN.
   - **Any other HEN:** have an ELF loader (elfldr) listening on port 9021. The app then opens
     `/data` itself with its bundled helper.

   No HEN has to grant JIT memory: the recompilers make their own when it isn't granted. The
   [firmware and setup guide](docs/HEN-SETUP.md) has the details for every firmware and HEN, and
   **Settings > Diagnostics** shows what the app got.
4. Add your games (see [Game files](#game-files)).
5. Start PS5CEMU-HAR from the home screen. The first time, the **Setup check** shows what's ready
   (storage, the recompilers, each side's games and keys, the 3DS's system files, box art) and what
   to do about anything that isn't.

**Updating:** when a newer release is out, the app asks whether to install it, and does it itself
(**Settings > Online** checks again). By hand: copy the new `PPSA99360` folder over the
old one. Either way, everything in `/data/ps5cemu` is kept. The PS5 keeps the app's name, icon and
background from when it was first registered; register it again in your loader to see new ones.

## The launcher

| Button | Action |
|---|---|
| L1 / R1 | Library and Settings |
| Touchpad click | The next side: Wii U, 3DS, DS (in the Library and Settings) |
| Cross | Play the game, or open what's focused |
| Options | A game's options: its hub, its game settings, favourite |
| Square / Triangle | Sort and search (Library); a setting's help (Settings), and Square back to Default (game settings) |
| R2 | Jump by letter (Library) |
| Circle | Back |

The app opens on the Library of the side a game came back from, or else the side you used last,
with the game you played last in focus. **Settings > Launcher > Start on: Ask each time** brings back
a chooser at start. You can switch sides without the app restarting. Each emulator's core only starts
with one of its games, so the emulators never run at the same time, and the launcher's background
work stops before a game starts.

- **Library:** every game's box art from [GameTDB](https://www.gametdb.com/) in a grid (its icon
  until the cover arrives), sorted by when you last played it, with filters, sorting and search
- **Game pages:** a game's description, developer, publisher, release date, genre, players and
  rating from GameTDB, and how it runs from the [compatibility list](docs/COMPATIBILITY.md); a Wii U
  game's graphic packs
- **Settings:** the side you're on and nothing else, one short list of pages with icons (Graphics,
  Screens, Audio, Controls, System, Game files, Online, Launcher, Diagnostics, About), a line under
  each setting saying what it does, and Triangle for more.
- **Game settings:** Options on a game, then **Game settings**: the same pages for that game alone.
  Each value shows **Default** (its side's) until you change it, Square puts it back, and the in-game
  menu saves its changes to the game's settings when it has some.
- **Music and menu sounds:** an original setup theme; **Settings > Audio** sets its volume or turns
  it off

## Wii U (Cemu)

- Cemu's x64 recompiler, at the PS5's 3840x2160 output, with a choice of upscaling filter (Bicubic
  by default), 120 Hz on displays that support it, and frame pacing (**Settings > Graphics**, or the
  in-game menu's Graphics) that holds a game to an even 60 fps at 120 Hz or 30 fps at 60 Hz.
- Plays WUA, WUD/WUX and unpacked games from any folder the PS5 can read.
- The Cemu community graphic packs, organized by folder like Cemu's Graphic Packs window, with a
  dropdown for each preset.
- Player 1's DualSense is the GamePad and other signed-in players get Pro Controllers, up to four
  players. Each player can be a GamePad, Pro Controller, Classic Controller, or Wii Remote with or
  without a Nunchuk, with motion controls, rumble, stick deadzones and button mapping.
- The TV or the GamePad screen as the main picture, with the other one in a corner if you want; the
  touchpad works as the GamePad's touch screen.
- Updates and DLC: installed from **Settings > Game files > Install updates and DLC**, or picked up automatically
  from your game folder or a WUA.
- Amiibo, from the in-game menu (see [Amiibo, save states, cheats and mods](#amiibo-save-states-cheats-and-mods)).
- Text entry with Cemu's on-screen keyboard.

## Nintendo 3DS (Azahar)

- Based on Mihawk's PS5 build of Azahar, with dynarmic's ARM recompiler. A New 3DS is emulated.
- 1x to 10x internal resolution (6x, 2400x1440, by default), texture filters (Anime4K, Bicubic,
  ScaleForce, xBRZ, MMPX), and shaders compiled in the background so games don't stutter.
- Screen layouts: a large top screen with the bottom one beside it, the top screen only, side by
  side, or stacked; either screen can take the main spot.
- **Borders:** Midnight, Waves, Aurora, Shell or PS5CEMU-HAR artwork around the screens in every layout, from
  **Settings > Screens** or the in-game menu.
- DualSense: A on Circle and B on Cross like a real 3DS (or swapped), circle pad and C-stick on the
  sticks, ZL/ZR on L2/R2, motion from the gyro and accelerometer, and every button remappable in
  **Settings > Controls**. The touchpad is the bottom screen.
- Save states, cheats and amiibo in the in-game menu, plus custom textures and mods.
- System region, language and CPU clock in **Settings > System**, with the 3DS Home Menu.
- Install CIA files (games, updates and DLC) from **Settings > Game files > Install a CIA file**.
- **Artic Base:** play a game straight from your own 3DS over the network. Start Artic Base on the
  3DS, open **Settings > Artic Base** and enter the address the 3DS shows. The same page runs the Artic Setup Tool, which copies your 3DS's system files.
- An on-screen keyboard for games that ask for text.

## Nintendo DS (melonDS)

New in the 3.5.1 beta: a side of its own, in melonDS's green, to the right of the 3DS.

- melonDS 1.1's core, with its ARM recompiler in the executable memory the PS5 gives (its
  interpreter otherwise) and its software renderer on a thread of its own.
- The two screens at 4K, with a **Sharp**, **Smooth** or **Square pixels** filter, in the same layouts
  and borders as the 3DS side's, set apart from them (**Settings > Screens**).
- DualSense: A on Circle and B on Cross like a real DS, the left stick as a second D-pad, every
  button remappable (**Settings > Controls**), the touchpad as the touch screen, and R3 held to blow
  into the microphone.
- Save states (five slots), melonDS's `.mch` cheats and the same in-game menu as the 3DS side's.
- No BIOS needed: melonDS's own replacements run most games. Your own DS's `bios7.bin`, `bios9.bin`
  and `firmware.bin` in `/data/ps5cemu/melonds/bios` are used when they are there
  (**Settings > System**).
- Not there: DSi games, the DS's wireless and its GBA slot.

## Game files

### Wii U

Put games in `/data/ps5cemu/games`, or pick another folder in **Settings > Game files** on the Wii U side.

```text
<game folder>/
├── Game.wua                        # Wii U archive: game, update and DLC in one file
├── Game.wux                        # or Game.wud
└── Game/                           # unpacked game
    ├── code/
    ├── content/
    └── meta/
```

Encrypted WUD and WUX dumps also need their disc keys in `/data/ps5cemu/keys.txt`.

### Nintendo 3DS

Put games in `/data/ps5cemu/azahar/games`, or pick another folder in **Settings > Game files** on the 3DS side.

- `.3ds`, `.cci`, `.cxi` and `.app` dumps, `.3dsx` and `.elf` homebrew, and Azahar's compressed
  formats (`.z3ds`, `.zcci`, `.zcxi`, `.z3dsx`) play directly.
- `.cia` files are installed first from **Settings > Game files > Install a CIA file**; updates and DLC too. Azahar
  only installs fully decrypted CIAs, the game inside included.
- Encrypted dumps need `aes_keys.txt` from your own console in `/data/ps5cemu/azahar/sysdata`.

### Nintendo DS

Put games (`.nds`) in `/data/ps5cemu/melonds/games`, or pick another folder in **Settings > Game
files** on the DS side. A save (`.sav`) beside a game is taken the first time it starts; from then on
saves are in `/data/ps5cemu/melonds/saves`.

No games, keys, firmware or other copyrighted console data are included. Dump them from hardware and
software you own, and don't download or share them.

## Controls

| In a Wii U game | Action |
|---|---|
| Touchpad | GamePad touch screen cursor: click to tap, hold the click to drag |
| Touchpad click + Options | Open the in-game menu |
| Touchpad click + L1 | Switch the main screen between TV and GamePad |
| Touchpad click + R1 | Show or hide the second screen in the corner |

| In a 3DS game | Action |
|---|---|
| Touchpad | Bottom screen cursor: click to tap, hold the click to drag |
| Touchpad click + Options | Open the in-game menu |
| Touchpad click + L1 | Swap the screens |
| Touchpad click + R1 | Next screen layout |

The in-game menu opens on the left in the launcher's look: the game's cover and badges, four quick
actions along the top, then the settings, each category showing its current value. Up and Down
move (past the list's top onto the quick actions), Right or Cross opens a category, Left and Right
change a setting, and Circle closes a category or goes back to the game. Changes are kept for your
next games. A 3DS game pauses while its menu is open.

- **Quick actions:** Wii U: Resume, Screens (swap TV and GamePad), Graphic packs, Amiibo. 3DS:
  Resume, Save state, Load state, Screens (next layout).
- **Both:** screens, picture, volume, controls, the performance overlay, and **Quit to the library**
  (hold Cross until the ring fills; unsaved progress is lost).
- **Wii U:** **Amiibo**, and a **Graphics** page with Cemu's **Accurate barriers** (on by default;
  off can be faster, but some games flicker) and **Async shader compile** (on by default; off waits
  for each new shader: stutter, but nothing drawn wrong).
- **3DS:** **Border**, **CPU clock**, **Speed limit** (100% by default; higher or None to
  fast-forward, for the current game only), and **Save states, cheats, amiibo**.

## Amiibo, save states, cheats and mods

- **Amiibo (both).** Put your own amiibo dumps (`.bin`) in `/data/ps5cemu/amiibo`. When a game asks
  for one, open the in-game menu: on the Wii U, **Amiibo** (Left and Right choose, Cross scans); on
  the 3DS, **Save states, cheats, amiibo > Amiibo**, then **Take the amiibo away** when the game is
  done. 3DS games can only write to an amiibo with your console's `aes_keys.txt`. No amiibo files
  are included.
- **Skylanders, Disney Infinity and LEGO Dimensions (Wii U).** **Settings > USB devices** switches on
  Cemu's emulated Skylanders Portal of Power, Disney Infinity Base or LEGO Dimensions Toypad; it is
  plugged in when a game starts. Put your figure dumps in `/data/ps5cemu/figures/skylanders`,
  `infinity` or `dimensions`, then in the game open the in-game menu's **USB devices**: Left and Right
  put the next figure on a slot, and **Empty** takes it off. The game saves its progress into the dump,
  as it would on the toy. A real portal plugged into the PS5 isn't supported.
- **Save states (3DS).** Five slots per game; loading is held (hold Cross until the ring fills).
  States made by earlier PS5CEMU-HAR builds load too. Keep saving in the game as well.
- **Cheats (3DS).** Put a game's cheats in `/data/ps5cemu/azahar/cheats/<title ID>.txt` (16 hex
  digits, shown in the in-game menu), in the Gateway format desktop Azahar uses, and turn them on and
  off from the menu.
- **Custom textures (3DS).** Turn on **Settings > Graphics > Custom textures** (3DS) and put a pack in
  `/data/ps5cemu/azahar/load/textures/<title ID>/`.
- **Mods (3DS).** LayeredFS mods go in `/data/ps5cemu/azahar/load/mods/<title ID>/`, with `romfs/` and
  `exefs/` inside.

## Online

- **Box art:** the first time a game shows up, the app downloads its cover from GameTDB
  (`art.gametdb.com`) by the ID on the game's box, in the background, into
  `/data/ps5cemu/covers/boxart`. A cover GameTDB doesn't have leaves a `.none` file there; delete it
  to try again. **Settings > Online** turns the downloads off.
- **Updates:** when the app starts fresh, it asks GitHub for the latest release. When a newer
  PS5CEMU-HAR is out, it asks whether to install it: **Update now** downloads the release, checks it
  against the release's SHA-256, puts its files in place of the old ones and starts the app again.
  Your games, saves and settings in `/data/ps5cemu` stay as they are. **Settings > Online >
  PS5CEMU-HAR updates** checks again.

Without an internet connection, the libraries just show the game icons.

## Where files are stored

Everything the app writes goes to `/data/ps5cemu`, except your game files:

```text
/data/ps5cemu/
├── ps5cemu.json                    launcher settings
├── settings.xml                    Cemu settings
├── controllerProfiles/             Cemu controller profiles
├── mlc01/                          Wii U storage: installed updates and DLC, saves
├── games/                          default Wii U game folder
├── keys.txt                        disc keys for encrypted Wii U dumps
├── graphicPacks/                   community graphic packs and your own
├── cache/                          Cemu shader and pipeline caches
├── amiibo/                         your amiibo dumps (.bin), for both emulators
├── figures/                        Skylanders, Disney Infinity and LEGO Dimensions dumps (Wii U)
├── azahar/
│   ├── games/                      default 3DS game folder
│   ├── sdmc/                       3DS SD card: installed CIAs, saves, extra data
│   ├── nand/                       3DS system storage
│   ├── sysdata/                    aes_keys.txt, if you add it
│   ├── cheats/                     3DS cheats, <title ID>.txt
│   ├── load/                       3DS custom textures (textures/) and mods (mods/)
│   ├── shaders/                    Azahar shader cache
│   └── log/azahar_log.txt          Azahar log
├── melonds/
│   ├── games/                      default DS game folder
│   ├── saves/                      DS saves, <game file>.sav
│   ├── states/                     DS save states
│   ├── cheats/                     DS cheats, <game file>.mch
│   └── bios/                       your DS's bios7.bin, bios9.bin and firmware.bin, if you add them
├── covers/                         game icons and box art (boxart/)
├── log.txt                         Cemu log
└── logs/                           app logs: boot.log, and boot.prev.log to boot.4.log for the
                                    four sessions before, each with its Cemu log (cemu.prev.txt...)
```

**Wii U saves from Cemu on PC:** with the game closed, copy
`mlc01/usr/save/00050000/<title ID>/user/<account>` from your PC to
`/data/ps5cemu/mlc01/usr/save/00050000/<title ID>/user/80000001`.

**3DS saves from Azahar or Citra on PC:** copy
`sdmc/Nintendo 3DS/<ID0>/<ID1>/title/00040000/<title ID>/data` to the same path under
`/data/ps5cemu/azahar/sdmc/Nintendo 3DS/`. ID0 and ID1 are all zeros in Azahar, on PC and PS5.

## Reporting problems

Check the [compatibility list](docs/COMPATIBILITY.md) first, then
[open an issue with the bug report form](https://github.com/premohq/PS5CEMU-HAR/issues/new/choose).
It asks for your firmware, HEN, app version and logs, which almost every problem needs.

**Settings > Diagnostics** has what a report needs:

- The app version, the firmware, and what the HEN gave the app.
- **Copy logs to USB:** puts the logs and settings in a dated `PS5CEMU-HAR-logs-...` folder on a USB
  drive. The app keeps the last five sessions' logs, so copy them soon after a problem and attach
  them to your report.
- **Clear shader caches** (hold Cross): for a game that crashes on a bad or shared cache.
  Games build them again as they run.

The boot log also gets a `[memory]` line once a minute, a `[perf]` (Wii U) or `[perf3ds]` (3DS) line
every 10 seconds with the frame rate, and `[crash]` lines if an emulator crashes.

## What's new in the 3.5.1 beta

- **3.5.0 starts again on older firmwares and kstuff setups** (#34). Where the HEN leaves a drive out
  (kstuff and ShadowMount+ on 5.50 and 8.60 among them), the app's sandbox helper opens it, and 3.5.0
  then still looked for its own files where the sandbox had kept them: the launcher found no font and
  never drew. It now looks for them again after the helper runs.
- **Nintendo DS games, on melonDS**, as a third side to the right of the 3DS, in green (see
  [Nintendo DS](#nintendo-ds-melonds)).
- **No Home screen:** every side opens on its Library, the game you played last in focus.
- **Settings, simple again:** only the side you're on, one short list of pages with icons, as 3.0.0
  had it.
- **Game settings:** any game can have its own graphics, screens, sound and system settings
  (Options on the game, then Game settings).

## What's new in 3.5.0

- **One launcher for both sides.** It opens on the Home of the side you used last, a touchpad click
  switches between Wii U and 3DS, and Settings holds both sides' pages. The Library has filters,
  sorting, search and jump by letter, and each game has a page with its details and graphic packs.
  There's no start screen to pick a side any more (**Settings > Display > Start on** brings back a
  chooser if you want one).
- **Setup check:** on the first start and in Settings, what's ready (storage, the recompilers, each
  side's games and keys, the 3DS's system files, box art), what to do about the rest, and a QR code
  to the guide.
- **New in-game menus** in the launcher's look: the game's cover, four quick actions (Wii U: Resume,
  Screens, Graphic packs, Amiibo; 3DS: Resume, Save state, Load state, Screens), each category's
  current value, a volume slider. Quitting and loading a state are held to confirm.
- **3DS save states from earlier builds load.** Every state made by an earlier PS5CEMU-HAR build
  used to say "Made by another version"; now they load. Loading a state also no longer crashes the
  app now and then.
- **A newer graphics driver:** 120 Hz output that reaches the games, and frame pacing that holds a
  Wii U game to an even 60 fps at 120 Hz or 30 fps at 60 Hz (**Settings > Graphics**).
- **The release ZIP installs as it is.** Every file in it can run, so a file manager that keeps the
  ZIP's permissions no longer installs an app the PS5 refuses with CE-107750-0
  ([#22](https://github.com/premohq/PS5CEMU-HAR/issues/22)).
- **Install it on EXT 1 or USB:** the app finds its files wherever ShadowMountPlus mounts it from,
  and updates and the restart after each game use that copy
  ([#22](https://github.com/premohq/PS5CEMU-HAR/issues/22),
  [#26](https://github.com/premohq/PS5CEMU-HAR/issues/26)).
- **3DS games with a cheat file start** instead of closing the app
  ([#20](https://github.com/premohq/PS5CEMU-HAR/issues/20)).
- **Graphic packs that replace game files** no longer close the app as the game starts
  ([#24](https://github.com/premohq/PS5CEMU-HAR/issues/24)).
- **Pokémon X and Y's moving black dots:** a switch to try, `"invariantPosition": true` in the
  `n3ds` section of `/data/ps5cemu/ps5cemu.json`
  ([#23](https://github.com/premohq/PS5CEMU-HAR/issues/23)).
- **A firmware and setup guide** for any PS5 that runs homebrew ([docs/HEN-SETUP.md](docs/HEN-SETUP.md)).

## What's new in 3.0.0

- **Updates from inside the app.** When a newer release is out, the app offers to download and install
  it, then starts again as the new version.
- **A new launcher.** Box art first and fewer words: Home, Library and Settings along the top (L1
  and R1), a grid of covers, and a page for each game with its description, developer, publisher,
  release date and rating from GameTDB, and its status from the compatibility list.
- **New in-game menus:** a panel on the left with the game's box art and details, its settings in
  categories that open in place.
- **Skylanders, Disney Infinity and LEGO Dimensions** on Cemu's emulated portals: switch one on in
  Settings > USB devices, and put figures on and take them off from the in-game menu
  ([#18](https://github.com/premohq/PS5CEMU-HAR/issues/18)).
- **3DS games run where the console refuses the recompiler's memory,** on Azahar's interpreter
  (slower), instead of not at all; Settings > Diagnostics says which.
- **Graphic packs in the Wii U in-game menu:** turn packs on and off and change their presets while
  the game runs, as Cemu's Graphic Packs window does (packs that replace textures apply at the next
  start).
- **3DS games pause** while the in-game menu is open.
- **3DS Home Menu** (Settings > System), from your console's files once Artic Setup has run.
- **Wii Remote pointer:** a Wii Remote points where a finger rests on the touchpad, or where you aim
  the DualSense.
- **GamePad speaker:** the sounds games play on the GamePad, from the DualSense's speaker
  (Settings > Audio).
- **Community graphic packs update** from GitHub (Settings > Online).
- **CIA installs can be canceled** with Circle.
- **A PS5CEMU-HAR border** for 3DS games: the Wii U side's bubbles meeting the 3DS side's waves.
- **New home screen art and icon** on the PS5, in the banner's design.
- **The launcher draws its rounded panels itself,** so they show without seams on the console.

<details>
<summary><strong>Older releases</strong></summary>

- **2.0.0 D:** Breath of the Wild's runes no longer freeze the game
  ([#10](https://github.com/premohq/PS5CEMU-HAR/issues/10)); Batman: Arkham Origins plays with its
  60 FPS pack off ([#15](https://github.com/premohq/PS5CEMU-HAR/issues/15)); 3DS borders; 3DS save
  states, cheats and amiibo and Wii U amiibo in the in-game menus; no restart when switching sides;
  an update notice; the HEN setup guide ([#12](https://github.com/premohq/PS5CEMU-HAR/issues/12));
  logs from the last five sessions.
- **2.0.0 C:** a new launcher look with music and menu sounds; Cemu's recompiler without etaHEN's
  JIT; USB and extended drives opened with the bundled helper; one emulator at a time; a 3DS
  on-screen keyboard, region setting and speed limit; Wii U Graphics settings; Diagnostics with logs
  to USB and shader cache clearing; a bug report form and compatibility list.
- **2.0.0 B:** 3DS sound at the right rate, a 3DS memory leak fixed, Artic Base, clearer CIA
  installs, a 3DS CPU clock setting, shared Wii U shader caches no longer crashing, and Accurate
  barriers in the Wii U menu.
- **2.0.0:** Azahar for 3DS games, a start screen to choose Wii U or 3DS, game icons and box art,
  per-player controls in the in-game menu, and memory leak fixes.
- **1.0.0:** dark blue launcher, graphic pack presets, controller settings, and update and DLC
  installs.
- **0.2.0:** games run at the right speed, GamePad screen options, touchpad cursor, in-game menu and
  on-screen keyboard.
- **0.1.0:** first version that ran on a PS5.

</details>

## Known issues

- Going back to the library restarts the app, and a Wii U game keeps running behind the in-game
  menu.
- Batman: Arkham Origins needs its 60 FPS graphic pack turned off; with it on, it glitches and
  crashes once gameplay loads ([#15](https://github.com/premohq/PS5CEMU-HAR/issues/15)).
- 3DS camera, microphone, local wireless and online play aren't supported.
- 3DS texture filters are expensive at high internal resolutions; if a game stutters, try Texture
  filter: None first.
- The first start with a large Wii U shader cache takes a few minutes while Cemu builds the
  pipelines.
- etaHEN is the most tested HEN; OnionHEN and others have had fewer reports so far.

## Building

Run `make release` on Linux (Ubuntu 24.04; WSL works too). It downloads every dependency at its
pinned version, builds Azahar, Cemu and the launcher, and writes the app to `build/app/PPSA99360` and
a ZIP to `dist/`. See [docs/BUILDING.md](docs/BUILDING.md), or run `make help` for the other targets.

- `port/`: the PS5 platform layer, both emulators' PS5 frontends, the launcher and the in-game menus
- `patches/`: changes to Cemu and Azahar
- `tools/`: build, packaging and artwork scripts
- `docs/`: building, the firmware and HEN setup guide, the compatibility list, and the
  [UI design](docs/UI-REDESIGN.md) the launcher and in-game menus are moving to

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
  the launchers' layout, artwork and software renderer are based on, the
  [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) for
  the app runtime, packaging and sandbox elevation, and the
  [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) designs the home screen
  follows
- The [Lexend](https://github.com/googlefonts/lexend) authors for the launchers' font (SIL Open Font
  License 1.1)
- **Dimok** for the Wii U Homebrew Launcher, whose background is used on the Wii U side
- The authors of the **3DS Homebrew Launcher**, which inspired the 3DS side's background
- **John Törnblom** for the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), and
  **pacbrew** for their PS5 libraries
- **Swordpdf** for the etaHEN jailbreak request from PS5SX2
- [GameTDB](https://www.gametdb.com/) and its contributors for the box art
- The authors of the [Cemu community graphic packs](https://github.com/cemu-project/cemu_graphic_packs)

## License

The app's own code is licensed under GPL-3.0-or-later (see [LICENSE](LICENSE)). Files derived from
Cemu keep Cemu's MPL-2.0 license, as noted in their headers. Azahar is GPL-2.0-or-later and melonDS
GPL-3.0, and the app that includes them is distributed under GPL-3.0-or-later. Other third-party components keep their own
licenses.

## Disclaimer

- **No affiliation.** This is an independent homebrew project. It is not affiliated with, endorsed
  by, or sponsored by Sony Interactive Entertainment, Nintendo, the Cemu project, the Azahar project
  or the melonDS project. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment
  Inc.; "Wii U", "Nintendo 3DS" and "Nintendo DS" are trademarks of Nintendo.
- **No proprietary material.** No Sony or Nintendo SDK, firmware, encryption keys, games or
  decrypted system modules are included.
- **No warranty.** This project is provided "as is", without warranty of any kind, to the extent
  permitted by law. See sections 15 and 16 of the GPL.
- **Use at your own risk.** Running homebrew requires a modified console, which may void its
  warranty, breach the platform's terms of service, or cause data loss.
- **Legal use only.** Only use it with hardware, accounts and content you own. This project does not
  support or enable piracy.
