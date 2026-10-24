# PokeDaisy for BananaOS (early preview)

A port of PokeDaisy to [BananaOS](https://github.com/JorreFog/BananaLinux), the
Linux for the Anbernic RG DS and RG DS Plus: the game runs on the top panel
(libmgba, the same mGBA 0.10.5 submodule as the Android app and the 3DS port)
and the companion lives on the bottom touchscreen. Like the 3DS port it is a
rewrite, not a recompile: it runs the same portable C core (`../3ds/core`,
the companion's reading and drawing) inside a host made for BananaOS.

**Status: early preview.** It builds, and runs headless on the fixture ROMs
with every check below passing (`make test`); the KMS, input and sound paths
follow BananaOS's emulator contract but have not been run on an RG DS yet.

## What it does

- **The panels**: KMS on both, straight from the kernel's DRM interface (no
  compositor, no libdrm): DRM master, both panels' modes as they are (never a
  timing change), a page flip per frame on the top panel, and the game paced by
  its vblank. Given back on exit.
- **The game**, on the top panel in three SCREEN modes, named in SETTINGS by
  what they are on your panels: **PIXEL** (the largest whole scale: 2x on the
  RG DS, 4x on the Plus), **SHARP** (fills the width, sharp bilinear: 2.7x /
  4.3x; the default) and **STRETCH** (the whole panel).
- **The companion** on the bottom panel, the 3DS port's (its README lists the
  tabs: PARTY, BAG, MAP, GUIDE, POKéDEX, BATTLE, SETTINGS, the TRAINER CARD) at
  2x on the RG DS and 3.2x on the Plus. Touch it, as on the 3DS.
- **Sound** through ALSA's `default`, the route bananad keeps (speakers,
  headphones, the Plus's amplifiers), S16 stereo 48 kHz.
- **Saves**: the same file RetroArch keeps for the game, `/data/saves/<game>.srm`
  (raw save data), so you can switch between PokeDaisy and gpSP / mGBA and keep
  playing. (For Emerald, whose cartridge has a clock, mGBA keeps the clock in 16
  bytes after the save data; RetroArch's cores read the save and leave those.)
  A save next to the ROM (`<game>.sav` or `.srm`, from another firmware) is
  copied in the first time, never moved. An in-game save is flushed to the card
  as soon as mGBA has written it. Before every start and every state load the
  save is backed up to `/data/pokedaisy/backups/` (newest 10 per game; none when
  the newest backup already holds the same bytes, so retrying a state can't push
  an older save out).
- **Save state**: one slot per game, `/data/states/gba/<game>.pd1`. A state
  never carries the save data, so loading one can't write an old copy over
  your save; it is written beside the old one and renamed over it.
- **Sleep**: closing the lid stops the game (bananad's SIGSTOP) and opening it
  carries on. **Shutdown while playing** (SIGTERM): the save is written and
  flushed to the card before it exits.
- Settings in `/data/pokedaisy/settings.ini`, the log in
  `/data/banana/logs/pokedaisy/` (the launcher keeps the last two).

## Controls

| | |
|---|---|
| D-pad / left stick, A, B, L, R, Start, Select | the GBA's buttons |
| Touchscreen | the companion |
| X | next companion tab |
| Y | companion back (a summary, an entry, a pick-list) |
| F (alone) | companion SETTINGS (again: back to the tab you were on) |
| R2 (held) / L2 | fast-forward while held / on and off (2x to 4x in SETTINGS; muted) |
| F + R / F + L | save state / load state |
| F + Start | quit to the menu (your in-game save is already on the card; unsaved progress isn't kept) |

The volume keys and F + volume stay bananad's.

## Games

As the 3DS port: retail English **FireRed / LeafGreen** and **Emerald**. On
BananaOS a GBA game the companion doesn't know runs in RetroArch as before:
choosing PokeDaisy for the Game Boy Advance (Settings > Other systems) costs
nothing for your other games.

## In BananaOS

BananaLinux carries it as `buildroot/package/pokedaisy` (this folder at a
pinned commit, with mGBA 0.10.5 as a second download) and starts it with
`/usr/libexec/banana/run-pokedaisy`. Settings > Other systems > Game Boy
Advance gets a third choice, **PokeDaisy**. banana-ui then asks
`pokedaisy --probe` about each GBA game as it starts it: a game the companion
knows (exit 0) gets PokeDaisy and both panels; any other (exit 2, or a zip)
goes to RetroArch's gpSP as before, with BananaOS's game panel on the other
screen. BananaLinux's docs/EMULATORS.md has the details.

What the launcher passes (each has the value shown as its default):

```
POKEDAISY_TOP      DSI-2 (BANANA_TOP)           the top panel's connector
POKEDAISY_TOUCH    fe5e0000.i2c (BANANA_TOUCH)  part of the bottom touchscreen's sysfs path
POKEDAISY_DATA     /data/pokedaisy              settings.ini
POKEDAISY_SAVES    /data/saves                  <game>.srm
POKEDAISY_STATES   /data/states/gba             <game>.pd1
POKEDAISY_BACKUPS  /data/pokedaisy/backups
POKEDAISY_SOUND    default                      the ALSA device
```

## Building and testing

Needs CMake, a C compiler and the mGBA submodule
(`git submodule update --init third_party/mgba`); nothing else to link but
libc (ALSA is loaded at run time).

```sh
cd banana
make            # build/pokedaisy for this computer
make test       # the fixture ROMs, then the port headless on them
```

- **`make fixtures`** builds the 3DS port's fixture test ROMs
  (`3ds/test/fixture_rom.c`: a few lines of GBA homebrew that copy one of the
  repo's RAM fixtures into the GBA and idle, so the companion reads the
  fixture's save as if it were the game) with clang and lld instead of
  devkitARM (`test/crt0.s`, `test/gba.ld`, `test/mkheader.py`; no logo in the
  header, none needed). They hold no game code or data.
- **`make test`** (`test/run.sh`) runs the port headless (`--headless DIR`, no
  panels: both drawn in memory) through scripted taps and buttons, on the RG
  DS's 640x480 and the Plus's 1024x768, writing both panels as PNGs into
  `build/shots/`, and checks a player's data: another firmware's save copied
  in and left in place, its bytes and size kept, the backup (and no duplicate
  ones from repeated state loads), F + R / F + L,
  SIGTERM, no half-written files, the screen mode kept across runs, and
  `--probe`'s answers. Look at the shots after any change you can see.
- The script language (`--script`): `wait:N` frames, `press:A` or
  `hold:F+START:N`, `tap:X,Y` (the companion's 320x240 pixels), `shot:NAME`,
  `term` (SIGTERM). The tab chips' centres are at y 13: PARTY 27, BAG 78, MAP
  129, GUIDE 180, DEX / BATTLE 231, fast-forward 273, SETTINGS 303.
- MAP, POKéDEX and GUIDE's wild / BOSS parts read the ROM: with the decomps
  built (`make -C ../3ds decomps fixture-roms`), run the port on
  `../3ds/build/fixture-*-rom.gba` the same way.

## Layout

| Path | What |
|---|---|
| `src/main.c` | the game loop, the buttons, saves and states, `--probe`, the headless script |
| `src/display.c` | the two panels over DRM ioctls (dumb buffers, page flips), or in memory |
| `src/scale.c` | the game and the companion onto a panel: whole scale or sharp bilinear |
| `src/input.c` | evdev: the pad, keyboards, the bottom touchscreen (as banana-ui finds them) |
| `src/audio.c` | ALSA, loaded with dlopen |
| `src/files.c` | the card's folders, copies, the save backups |
| `test/` | the fixture ROM build (clang + lld) and `run.sh` |

The companion's sources are the 3DS port's and stay in `../3ds/core`: a change
there is a change for both ports (`make -C ../3ds preview` still renders it).
The one thing added for this port is `pd_screen_mode_set_names`, so a host
names SCREEN's modes by its own scales.

## Next steps

- Run it on an RG DS and an RG DS Plus: the panels, the touch mapping, sound,
  sleep and a shutdown mid-game; measure the frame time.
- The rest of the app's games and features, as they reach the portable core
  (Ruby / Sapphire, ROM hacks, other languages, Game Boy).
- Test with a ROM that writes its save (the fixture ROMs never do).
- A resume state on F + Start, with Settings' Quick resume (the app's
  `pkStateMatchesSave` check first).
