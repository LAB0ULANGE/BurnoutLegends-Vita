# Burnout Legends 2.0 — native PC (Windows)

Burnout Legends (PSP, ULES00125) statically recompiled to x86-64 and run natively on
Windows: the game's own code, translated ahead of time, with a high-level emulation of
the PSP system calls, an OpenGL renderer for the GE display lists, sound, movies, pad
and keyboard. It is the same runtime as the PS Vita port (Burnout Legends 2.0), with
its additions (BONUS 2.0 menu, picture filters, extra paints, Signature Takedown
awards).

## What you need

- Windows 10 or 11, a GPU with OpenGL 2.1+.
- **Your own copy of the game**: the European Burnout Legends disc image
  (ULES00125 v1.01, `.iso` or `.cso`). No game file is included.

## Playing

1. Put your disc image next to `burnout_legends.exe`.
2. Run `burnout_legends.exe`. The first launch makes the game files from your image
   into `data\` (a few seconds: copy, EBOOT decryption, music conversion).
3. Play. `settings.ini` (resolution, full screen, FXAA, sound) and `controls.ini`
   (keyboard) are made at the first launch.

| PSP | Pad (XInput) | Keyboard (default) |
|---|---|---|
| Cross | A | Space, Z |
| Square | X | X |
| Circle | B | Backspace, C |
| Triangle | Y | V |
| L / R | LB, LT / RB, RT | A, Q / E, Shift |
| Start / Select | Start / Back | Enter, Escape / Tab |
| D-pad, stick | D-pad, left stick | Arrows |

F11 or Alt+Enter: full screen. F12: screenshot.

Saves are in the PSP format (`data\memstick\PSP\SAVEDATA`): they can be copied to and
from a PSP, a PS Vita (Adrenaline) or PPSSPP.

## Building

From the repository root, with Visual Studio 2022:

```
cmake -S . -B out/burnout
cmake --build out/burnout --config Release --target burnout_pc
python profiles/burnout/pc/package_pc.py out/burnout/profiles/burnout/Release <folder>
```

The build expects the game files beside the repository (`../original/`) to generate
the recompiled code; see the main README.
