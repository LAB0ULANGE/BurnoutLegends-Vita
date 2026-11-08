# Burnout Legends for PS Vita (native recompilation)

![Burnout Legends on PS Vita](docs/images/livearea.png)

A native PS Vita port of **Burnout Legends** (PSP), made by statically recompiling
the game's PSP executable to ARM with [PSPRecomp](docs/PSPRecomp.md). It is not an
emulator: the game's code runs natively, with its own renderer (vitaGL), sound,
movies and saves rebuilt for the Vita.

*Version française : [README.fr.md](README.fr.md)*

> **You need your own copy of the game.** This repository contains no game file,
> no executable from the disc and no recompiled game code. Dump your own UMD.

## Screenshots

Taken on a PS Vita:

| | |
|---|---|
| ![Wall takedown](docs/images/takedown.jpg) | ![Main menu](docs/images/menu.jpg) |

## Features (0.0.1)

- 30 fps races at 960x544 (dynamic resolution keeps the frame rate on the
  heaviest tracks, the menus and the player's car stay at full resolution)
- Sound, music and the menu movies
- Native Vita save dialog
- **Your PSP / Adrenaline saves are used directly**: the saves already on your
  memory card (`ux0:pspemu`, `uma0:pspemu`, `imc0:pspemu`, `ur0:pspemu`,
  `xmc0:pspemu`) are listed, labelled "- PSP". Nothing to copy or convert, and a
  save written by the port can still be loaded in Adrenaline.
  A backup of your Adrenaline saves is made at first launch in
  `ux0:data/burnout/backup_adrenaline/`.

## Supported game version

| | |
|---|---|
| Game | Burnout Legends, Europe |
| Disc | **ULES00125**, DISC_VERSION 1.01 |
| Decrypted `EBOOT.BIN` SHA-256 | `89263a8f1f262971ad85761202fa6b02f4dfc86b3fbc733402ec20d0003e57c4` |

Other versions (US ULUS10025, other revisions) are not supported yet: the
recompiled code only matches this executable.

## Requirements

- A PS Vita / PS TV on HENkaku / Ensō, with [VitaShell](https://github.com/TheOfficialFloW/VitaShell)
- `libshacccg.suprx` (the Vita's shader compiler) in `ur0:data/external/`.
  If you do not have it, extract it with
  [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D) and copy it to
  `ur0:data/external/libshacccg.suprx`.
- Your own Burnout Legends disc image (**ULES00125**), ISO or CSO, the one you
  play with Adrenaline
- About 700 MB free on `ux0:`

## Installation

1. Put your Burnout Legends ISO or CSO in **`ux0:pspemu/ISO/`** (where Adrenaline
   looks for it; `uma0:`, `imc0:` and `xmc0:` work too). Any file name.
2. Install the VPK with VitaShell.
3. Start **Burnout recomp GL**. The first launch installs the game from your
   ISO by itself (copies the game files, decrypts the game, converts the music):
   about 10 minutes, only once. If it is interrupted, it resumes where it stopped. Then the game starts.

If no ISO is found, the game tells you so: copy it to `ux0:pspemu/ISO/` and start
again. Once installed, the port no longer needs the ISO (it has its own copy in
`ux0:data/burnout/`); keep it if you also play the game in Adrenaline.

Each launch then spends about a minute on the logo screen while the shaders are
compiled for your console.

## Controls

The PSP controls, on the same buttons. **START + SELECT** quits the game.

## Known issues

- Short slowdowns can happen on the largest tracks (Waterfront, Downtown) when a
  new part of the track streams in.
- About a minute of loading at each start (shader compilation).
- Only ULES00125 v1.01 is supported (the install checks it).

## Reporting a bug

Please attach the log: **`ux0:data/burnout-0.0.1.log`** (and `.log.prev`, the
previous run). Say which track and what you were doing. The log already
records memory use and every slow frame.

## Building from source

See [profiles/burnout/README.md](profiles/burnout/README.md). In short: decrypt
the executable, run the recompiler on it (the generated code stays on your PC),
then build the Vita target with VitaSDK.

## Credits and licences

- [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) by Jessica Natalia
  and contributors (MIT): the recompiler and runtime this port is built on.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) by Rinnegatamante (OpenGL on
  the Vita's GXM), used with a small patch (`profiles/burnout/tools/vitagl-*.patch`).
- PSP savedata encryption (`profiles/burnout/host/psp_savedata.cpp`) is ported
  from [PPSSPP](https://github.com/hrydgard/ppsspp) and is **GPL-2.0-or-later**;
  a Vita build that includes it is therefore distributed under the GPL.
- First-launch install: PPSSPP's PRX decrypter and libkirk (GPL-2.0) decrypt
  the game's executable, and the FFmpeg ATRAC3plus decoder (LGPL-2.1, PPSSPP's
  `at3_standalone`) converts the music.
- [stb_image](https://github.com/nothings/stb) (public domain).
- The rest of this repository is under the MIT licence ([LICENSE](LICENSE)).

Burnout and Burnout Legends are trademarks of Electronic Arts. This project is
not affiliated with or endorsed by Electronic Arts or Criterion Games.
