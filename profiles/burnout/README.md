# Burnout Legends profile

Static recompilation of Burnout Legends (PSP, PAL, ULES00125 v1.01) for the PS
Vita, with its own GE renderer on vitaGL (`host/ge_gl.cpp`), sound (SAS,
music, movies), native save dialog and PSP savedata encryption. The player's
guide is the [repository README](../../README.md); this page is for building.

The PC target (`burnout_headless`) runs the same game headless or with an
OpenGL window, for tests and profiling.

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
PSPRECOMP_NO_RETURN_HINTS=1 psp_recomp ../original/EBOOT_DECRYPTED.ELF --auto out/gen 0x08804000 16384
python profiles/burnout/tools/patch_libc_loops.py out/gen
python profiles/burnout/tools/patch_main_menu.py out/gen
python profiles/burnout/tools/patch_awards.py out/gen
python profiles/burnout/tools/patch_signatures.py out/gen
python profiles/burnout/tools/patch_paints.py out/gen
python profiles/burnout/tools/patch_native.py out/gen
python profiles/burnout/tools/patch_cars.py out/gen
python profiles/burnout/tools/patch_frame_rate.py out/gen
python profiles/burnout/tools/patch_trax.py out/gen
```

The unit span must be 16 KiB (16384): the runtime's fast unit index and the
dense entry dispatch assume it. `patch_libc_loops.py` replaces the game's
byte-per-iteration memcpy/memset loops with host calls. `patch_main_menu.py` lets the
host give the main menu its fifth row (BONUS) before the game builds it. `patch_awards.py` adds the Bonus
filters just unlocked to the game's own "new award unlocked" notice. Point the build at the
corpus with `-DBURNOUT_GENERATED_DIR=out/gen`.

## Build

PC (Visual Studio 2022):

```bash
cmake -S . -B out/burnout -G "Visual Studio 17 2022" -A x64 -DPSPRECOMP_PROFILE=burnout -DPSPRECOMP_BUILD_TESTS=OFF
cmake --build out/burnout --config Release --target burnout_headless
```

PS Vita (VitaSDK, Ninja). vitaGL must be built from source with
`tools/vitagl-2bdbe89-valid-region.patch` applied (commit 2bdbe89; it adds
`vglSetFramebufferValidRegion` and `vglSetFramebufferDepthKeep`) and installed
into VitaSDK:

```bash
cmake -S . -B out/vita -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake   -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard"   -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DPSPRECOMP_PROFILE=burnout   -DPSPRECOMP_BUILD_TOOLS=OFF -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_GENERATED_OPT_LEVEL=3   -DBURNOUT_GENERATED_DIR=out/gen -DBURNOUT_VITA_GL=ON   "-DBURNOUT_GENERATED_EXTRA_FLAGS=-fno-gcse;-fira-loop-pressure;-fno-tree-pre;-fno-code-hoisting;-fno-unswitch-loops;-fno-peel-loops"
ninja -C out/vita profiles/burnout/CMakeFiles/burnout-gl.vpk-vpk   # out/vita/profiles/burnout/burnout-gl.vpk
```

The LiveArea pictures, the bubble icon and the save icons are read from
`../original/` (`vita_icon0.png`, `vita_livearea/`, `save_icons/`); set
`BURNOUT_VITA_ICON`, `BURNOUT_VITA_LIVEAREA` and `BURNOUT_VITA_SAVE_ICONS` to
use others. `kRelease` in `host/main_vita.cpp` selects the public build (no
launch menu, no debug combos); set it to `false` for the test menu.

If `vita-elf-create` fails or the VPK will not install (error 0x800F1727), the
size of `burnout_text_pad` in `host/main_vita.cpp` has to move the end of the
text segment: keep it between 0x300 and 0x600 into its 4 KiB page.

## Game data layout

| PC (default paths, run from the repo root) | Vita |
|---|---|
| `../original/EBOOT_DECRYPTED.ELF` | `ux0:data/burnout/EBOOT_DECRYPTED.ELF` |
| `../original/PARAM.SFO` | `ux0:data/burnout/PARAM.SFO` |
| `../original/USRDIR/` | `ux0:data/burnout/USRDIR/` |
| (music converted by `tools/convert_music.py`) | `ux0:data/burnout/music_ima/` |

Saves: on the Vita the PSP memory stick is `ux0:pspemu` (Adrenaline's), saves
are encrypted like a PSP's; the PC uses `out/burnout-ms0`.

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

## Renderer switches (PC, `burnout_headless --gl`)

The decode worker's shortcuts can be turned off to compare captures and timings
(the `GE OpenGL time: decode .., draw ..` line at exit):

- `PSPRECOMP_BURNOUT_NO_CULL`: draw the objects a GE bounding box test (BBOX/BJUMP) finds out of view.
- `PSPRECOMP_BURNOUT_NO_FAST_PRIM`: every PRIM through the general decode path.
- `PSPRECOMP_BURNOUT_RGBA_UPLOAD`: paletted textures converted on the worker, not the GL thread.
- `PSPRECOMP_BURNOUT_PRIM_PATHS`, `PSPRECOMP_BURNOUT_FLUSH_STATS`: per-frame counts of PRIM paths and of what ends batches.
- `PSPRECOMP_BURNOUT_ALL_PAINTS`: every extra car paint offered (bonus menu runs).
- `PSPRECOMP_BURNOUT_ALL_CARS`: every car's unlock flag set (bonus menu runs).
- `PSPRECOMP_BURNOUT_NO_STATIC_VBO`: static geometry copied every frame instead of kept in GPU buffers.

The GE front end (`--async-ge`, on by default on the Vita) walks the display
lists on another thread, from a copy of each list's buffer taken when the game
queues it. On the PC its walk is too fast to meet the game's rewrites; these
make it as slow as on the Vita, with `--cache-sublists` as there:

- `PSPRECOMP_BURNOUT_FRONT_SLOW_MS=n`: a pause before each list's walk.
- `PSPRECOMP_BURNOUT_FRONT_SLOW_WALK_US=n`: a pause every 256 steps of the walk.
- `PSPRECOMP_BURNOUT_FRONT_OFF_AT=vblank`: the front end turns off there, as after a record desync.

## How the HLE models time

Threads are cooperative and switch at blocking imports. Time is virtual: guest
work costs no virtual time and, when every thread waits, the clock jumps to the
next event (vblank at 59.94 Hz, delay expiry, audio buffer drain). Host time
between two flips is therefore the CPU cost of a game frame (all threads, plus
HLE and the display-list scan), with no idle time included.

## Known limitations

- Only ULES00125 v1.01.
- Network and ad-hoc play are stubs.
- Some long flips on the largest tracks when new track sections stream in.
