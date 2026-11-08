# Burnout Legends for PS Vita (native recompilation)

A native PS Vita port of **Burnout Legends** (PSP), made by statically recompiling
the game's PSP executable to ARM with [PSPRecomp](docs/PSPRecomp.md). It is not an
emulator: the game's code runs natively, with its own renderer (vitaGL), sound,
movies and saves rebuilt for the Vita.

*Version française : [README.fr.md](README.fr.md)*

> **You need your own copy of the game.** This repository contains no game file,
> no executable from the disc and no recompiled game code. Dump your own UMD.

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

- A PS Vita / PS TV on HENkaku / Ensō (any firmware supported by h-encore or Ensō)
- [VitaShell](https://github.com/TheOfficialFloW/VitaShell)
- `libshacccg.suprx` (the Vita's shader compiler) in `ur0:data/external/`.
  Extract it with [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D) if you
  do not have it; copy it to `ur0:data/external/libshacccg.suprx`.
- Your own Burnout Legends UMD (ULES00125), dumped to an ISO
- On a PC: [PPSSPP](https://www.ppsspp.org/) (to decrypt the game's executable) and
  Python 3 with [PyAV](https://pypi.org/project/av/) (`pip install av`, plus
  `pip install audioop-lts` on Python 3.13 or newer) to convert the music

## Installation

### 1. Prepare the game files on a PC

1. **Decrypt the executable.** In PPSSPP, open `memstick/PSP/SYSTEM/ppsspp.ini`
   and set `DumpDecryptedEboots = True` in the `[General]` section (PPSSPP
   closed). Start the game once in PPSSPP: the decrypted executable is written to
   `memstick/PSP/SYSTEM/DUMP/ULES00125_Burnout.BIN`. Rename it `EBOOT_DECRYPTED.ELF`.
2. **Extract the disc.** Open the ISO with 7-Zip and extract `PSP_GAME/PARAM.SFO`
   and the whole `PSP_GAME/USRDIR` folder.
3. **Convert the music** (the Vita cannot decode the game's ATRAC3plus music):

   ```bash
   python profiles/burnout/tools/convert_music.py USRDIR/music music_ima
   ```

You now have:

```text
burnout/
  EBOOT_DECRYPTED.ELF
  PARAM.SFO
  USRDIR/
  music_ima/
```

### 2. Copy them to the Vita

Copy that `burnout` folder to `ux0:data/burnout/` (VitaShell FTP or USB).

### 3. Install the VPK

Install the VPK with VitaShell and start **Burnout recomp GL** from the LiveArea.

The first launch takes about a minute on the logo screen: the shaders are
compiled for your console.

## Controls

The PSP controls, on the same buttons. **START + SELECT** quits the game.

## Known issues

- Short slowdowns can happen on the largest tracks (Waterfront, Downtown) when a
  new part of the track streams in.
- About a minute of loading at each start (shader compilation).
- Only ULES00125 v1.01 is supported.

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
- [stb_image](https://github.com/nothings/stb) (public domain).
- The rest of this repository is under the MIT licence ([LICENSE](LICENSE)).

Burnout and Burnout Legends are trademarks of Electronic Arts. This project is
not affiliated with or endorsed by Electronic Arts or Criterion Games.
