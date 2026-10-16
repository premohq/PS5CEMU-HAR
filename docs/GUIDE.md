# PS5CEMU-HAR guide

Everything the [README](../README.md) leaves out: the launcher, each system, where games and files go,
the controls and in-game menus, the remote touch screen, saves, mods and what to do when something goes
wrong.

- [The launcher](#the-launcher) · [Wii U](#wii-u-cemu) · [3DS](#nintendo-3ds-azahar) · [DS](#nintendo-ds-melonds)
- [Game files](#game-files) · [Controls](#controls) · [Remote touch screen](#remote-touch-screen-a-phone-as-the-second-screen)
- [Amiibo, save states, cheats and mods](#amiibo-save-states-cheats-and-mods) · [Online](#online) · [Where files are stored](#where-files-are-stored)
- [Reporting problems](#reporting-problems) · [Known issues](#known-issues)

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
with the game you played last in focus. You can switch sides without the app restarting. Each emulator's core only starts
with one of its games, so the emulators never run at the same time, and the launcher's background
work stops before a game starts.

- **Library:** every game's box art from [GameTDB](https://www.gametdb.com/) in a grid (its icon
  until the cover arrives), sorted by when you last played it, with filters, sorting and search
- **Game pages:** a game's description, developer, publisher, release date, genre, players and
  rating from GameTDB, and how it runs from the [compatibility list](COMPATIBILITY.md); a Wii U
  game's graphic packs
- **Settings:** opens over the Library, for the side you're on and nothing else: one short list of
  pages with icons (Language, Graphics, Audio, Controls, Remote touch screen, Game files, Accounts and
  saves, Online, Launcher, Diagnostics, About, and the side's own: Screens, System, USB devices or
  Artic Base), a line under each setting saying what it does, and Triangle for more.
- **Game settings:** Options on a game, then **Game settings**: the same pages for that game alone.
  Each value shows **Default** (its side's) until you change it, Square puts it back, and the in-game
  menu saves its changes to the game's settings when it has some.
- **Music and menu sounds:** an original setup theme, or your own: put MP3 or WAV files in
  `/data/ps5cemu/music` and choose one, or *Shuffle your music*, in **Settings > Audio > Launcher
  music**, which also sets its volume or turns it off
- **Themes** (**Settings > Launcher > Theme**): Link Jack (the default); Classic, the launcher of 2.0
  with Continue playing, Recently played and lists beside the game in focus; Eden, after ProsperoEden's
  launcher; Meyer Oscar and Meyer Oscar Light (your recently played games first); Midnight (true
  black, for OLED screens); and two experiments, Phantom and Terminal. Your own: a JSON file in
  `/data/ps5cemu/themes`; the Theme row's help (Triangle) shows the format. **Settings > Launcher**
  also hides any side you don't use.
- **What's new:** on the first start and after an update, the app opens once on what the version
  brings.
- **Home screen tiles:** **Add to home screen** in a game's Options menu makes the game a tile of its
  own on the PS5's home screen (the open PS5 Forwarder Format, in `/data/homebrew`), with its box art.
  ShadowMount Plus shows it; starting it opens the game straight away and closes the app after it.
  Tiles need elfldr on port 9021, which the app also uses to put a tile in `/data/homebrew` when the
  sandbox can't. **Remove from home screen**, in the same menu, takes it away again.
- **Your language:** the menus follow the PS5's language; **Settings > Language** chooses another.

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
- Updates and DLC: **Settings > Game files > Updates and DLC** looks on every USB drive, extended
  storage, the games folder and `/data`, lists what it finds by game with what is installed now, and
  installs one with Cross or every new one with Square (Triangle picks a folder by hand). A game's own
  are in its Options menu too. Unpacked updates in your game folder and WUAs are picked up as they are.
- **Accounts** (**Settings > Accounts and saves**): up to twelve Wii U accounts, each with its own
  saves, or one for each PS5 user, so everyone who signs in to the PS5 plays on their own saves.
- **AMD FSR 1** as the fifth *Upscaling to 4K* filter, in Settings, a game's settings and the in-game
  menu: a sharper picture for a game drawn below 4K.
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
  The setup needs no keys from you: Azahar's own are built in, as in its official builds. An
  `aes_keys.txt` in `/data/ps5cemu/azahar/sysdata` takes the place of all of them, so leave it out
  unless it is complete; when it lacks what the setup needs, the 3DS's screen says so.
- An on-screen keyboard for games that ask for text.

## Nintendo DS (melonDS)

New in 4.0.0: a side of its own, in melonDS's green, to the right of the 3DS.

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
- Dumps must be decrypted too: Azahar plays no encrypted game, whatever keys it has. Decrypt them on
  your 3DS first (GodMode9).

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

## Remote touch screen: a phone as the second screen

Your phone, tablet or computer can be the game's touch screen: the Wii U GamePad's screen, the 3DS's
bottom screen or the DS's touch screen, in its browser, with touch. Nothing to install. (dolphin78's
idea and prototype, #38.)

1. Turn on **Settings > Remote touch screen** (on any side), or **Remote touch screen** in a game's
   menu (touchpad click + Options). It stays on for your next games.
2. Put the phone on the same network as the PS5 and scan the QR code shown there, or type the address
   under it (`http://<the PS5's address>:8080/`; the first free port from 8080).
3. Start a game: the page shows the second screen, even when the TV doesn't. Tap or drag on it to
   touch: player 1's GamePad on the Wii U, the bottom screen on the 3DS and the DS. The DualSense
   stays the buttons, and the sound stays on the PS5.

- **What it costs:** nothing while no browser shows it. While one does, the PS5 copies that screen up
  to 30 times a second (on the Wii U and the 3DS from the GPU), so a game can run a little slower.
- **Privacy:** any device on your network can open the page and touch. The traffic is plain HTTP on
  your local network. Turn it off on a network you share.
- **If the phone can't reach it:** guest Wi-Fi often keeps devices apart (client isolation); use the
  main network. The page shows "Waiting for video" until a game is running. A phone that goes to
  sleep or leaves the page lets go of its touch within a second, and reconnects when you come back.

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
- **Mods (Wii U).** Cemu's mods (graphic packs that replace a game's files, and the ones shared as
  `content` and `aoc` folders alone): put each in `ps5cemu/mods` on a USB drive, then **Options** on
  the game, **Add mods from USB**, and turn it on in the game's **Graphic packs**. The
  [mods guide](MODS.md) has the folders, title IDs and what to do when a mod does not work.
- **Mods (3DS).** LayeredFS mods go in `/data/ps5cemu/azahar/load/mods/<title ID>/`, with `romfs/` and
  `exefs/` inside.

## Online

- **Box art:** the first time a game shows up, the app downloads its cover from GameTDB
  (`art.gametdb.com`) by the ID on the game's box, in the background, into
  `/data/ps5cemu/covers/boxart`. A failed download is tried again a few seconds later; a cover
  GameTDB doesn't have is asked for again after two weeks. **Settings > Online** turns the downloads
  off, and has *Download missing box art* and *Download all box art again*.
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
├── music/                          your own launcher music (.mp3, .wav)
├── themes/                         your own launcher themes (.json)
├── log.txt                         Cemu log
└── logs/                           app logs: boot.log, and boot.prev.log to boot.4.log for the
                                    four sessions before, each with its Cemu log (cemu.prev.txt...)
```

**Backups:** **Settings > Accounts and saves > Back up to USB** copies every side's saves, save
states and the Wii U's accounts (never the games) to `PS5CEMU-HAR-saves/<date and time>` on the
first USB drive plugged in. **Restore from USB** puts a backup back, moving the saves there now to
`/data/ps5cemu/saves-before` first.

**Wii U saves from Cemu on PC:** with the game closed, copy
`mlc01/usr/save/00050000/<title ID>/user/<account>` from your PC to
`/data/ps5cemu/mlc01/usr/save/00050000/<title ID>/user/80000001`.

**3DS saves from Azahar or Citra on PC:** copy
`sdmc/Nintendo 3DS/<ID0>/<ID1>/title/00040000/<title ID>/data` to the same path under
`/data/ps5cemu/azahar/sdmc/Nintendo 3DS/`. ID0 and ID1 are all zeros in Azahar, on PC and PS5.

## Reporting problems

Check the [compatibility list](COMPATIBILITY.md) first, then
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
