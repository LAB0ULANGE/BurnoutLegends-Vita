# Burnout Legends profile (headless benchmark)

This profile answers one question: can a static recompilation of Burnout Legends
(PSP, PAL, ULES00125 v1.01) run its race loop within 33 ms per frame on a PS Vita?
It is deliberately headless: display lists are accepted and discarded, audio is
silent but paced, movies are not decoded. No renderer exists in this profile.

## Supported executable

| Item | Value |
|---|---|
| Disc | ULES00125, PAL, DISC_VERSION 1.01 |
| Input | `PSP_GAME/SYSDIR/EBOOT.BIN` decrypted by PPSSPP 1.17.1 (`DumpDecryptedEboots = True`) |
| SHA-256 | `89263a8f1f262971ad85761202fa6b02f4dfc86b3fbc733402ec20d0003e57c4` |
| Load base | `0x08804000` (relocatable PRX, module `Burnout`) |

No game file, decrypted executable, save or generated code is committed: the
generated corpus is a direct translation of the commercial executable and is
ignored by `profiles/burnout/.gitignore`.

## Regenerate the AOT corpus

```bash
psp_analyze ../original/EBOOT_DECRYPTED.ELF profiles/burnout/analysis/ULES00125_report.json 0x08804000
psp_recomp  ../original/EBOOT_DECRYPTED.ELF --auto profiles/burnout/generated 0x08804000 0x4000
```

The unit span must be 16 KiB (`0x4000`): the runtime's fast unit index and the
dense entry dispatch assume it. Expected output: 167 units, 234 import wrappers,
no unsupported instruction (`burnout_opcode_census` reports the census).

## Build

PC (Visual Studio 2022):

```bash
cmake -S . -B out/burnout -G "Visual Studio 17 2022" -A x64 -DPSPRECOMP_PROFILE=burnout -DPSPRECOMP_BUILD_TESTS=OFF
cmake --build out/burnout --config Release --target burnout_headless
```

PS Vita (VitaSDK, Ninja):

```bash
cmake -S . -B out/vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DPSPRECOMP_PROFILE=burnout \
  -DPSPRECOMP_BUILD_TOOLS=OFF -DPSPRECOMP_BUILD_TESTS=OFF
ninja -C out/vita -j10      # produces profiles/burnout/burnout-bench.vpk
```

## Game data layout

The benchmark needs the user's own files and a profile save. The save must be
unencrypted: create a profile in PPSSPP with `EncryptSave = False` in
`[SystemParam]` of `ppsspp.ini`.

| PC (default paths, run from the repo root) | Vita |
|---|---|
| `../original/EBOOT_DECRYPTED.ELF` | `ux0:data/burnout/EBOOT_DECRYPTED.ELF` |
| `../original/PARAM.SFO` | `ux0:data/burnout/PARAM.SFO` |
| `../original/USRDIR/` | `ux0:data/burnout/USRDIR/` |
| `out/burnout-ms0/PSP/SAVEDATA/ULES00125SAVE000/` | `ux0:data/burnout/ms0/PSP/SAVEDATA/ULES00125SAVE000/` |

## Running the benchmark

```bash
out/burnout/profiles/burnout/Release/burnout_headless.exe \
  --script profiles/burnout/scripts/race_world_tour_compact.txt --frames-csv out/frames.csv
```

The script loads the profile and starts World Tour > Compact > first event,
then holds CROSS (throttle) with periodic R (boost). The measurement window opens
10 s (600 vblanks) after `Tracks/.../streamed.dat` is first opened and lasts 60 s
of PSP time. The reported frame is the interval between two frame-buffer flips
(the game renders at 30 fps: two vblanks per frame).

On Vita the VPK runs the same race once at 444 MHz (the highest ARM clock an app can
request without plugins; a clock plugin's per-title profile overrides it) and writes
`ux0:data/burnout-bench.txt`; `ux0:data/burnout-bench.log` holds the progress
log. The screen is a solid colour: blue running, green finished, red error.
An optional `ux0:data/burnout/race.txt` replaces the built-in input script.

## How the HLE models time

Threads are cooperative and switch at blocking imports. Time is virtual: guest
work costs no virtual time and, when every thread waits, the clock jumps to the
next event (vblank at 59.94 Hz, delay expiry, audio buffer drain). Host time
between two flips is therefore the CPU cost of a game frame (all threads, plus
HLE and the display-list scan), with no idle time included.

## Known limitations

- Graphics: display lists are only walked for SIGNAL/FINISH callbacks; GE work
  (vertex processing, rasterisation) is not part of the measurement.
- Audio: SAS/ATRAC produce silence; the Media Engine work is not measured.
- Movies end as soon as the game has streamed the file in.
- Savedata loads/saves plain files under ms0:; list dialogs pick the first save.
- Network, ad-hoc and the remaining sceMpeg/sceUtility calls are logging stubs.
