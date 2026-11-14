"""Burnout Legends (ULES00125): frame rate hooks.

- 0x088360A4 (a1: 1 = 30 frames a second, 0 = 60) is called at screen
  changes; it sets the vblanks per frame (0x08855C50) and the time step
  (0x08A4A144: 1/rate s at 0x08ED6AFC, 1000/rate ms at 0x08ED6B00). Menus
  ask for 30, races for 60. The host (burnout_frame_rate) logs it.
- The vblank handler 0x08855374 stores 2 vblanks per frame (0x088553F4) while
  the byte at 0x08DCC999 is 0, as in races: the race steps 1/60 s at a time
  but shows a frame every second vblank. The host
  (burnout_vblanks_per_frame) may store 1 instead while the time step is
  1/60 s. (The 60 FPS CWCheat removes the vblank skip at 0x08855434 and
  halves a constant instead, which also runs the menus at twice their speed.)
- The main loop runs the steps of 1/60 s at +0x32FAC of the game object
  each frame (2 in races at 30 frames a second); 0x0880F48C stores the next
  frame's count. At 60 frames a second it stays 2 and the race runs twice as
  fast, so the host (burnout_frame_steps) gives the vblanks gone since the
  last frame instead (1 at 60, more when the frame is late).

usage: python patch_frame_rate.py GENERATED_DIR   (patches generated_unit_0002.cpp, 0012 and 0020 in place)
"""
import pathlib
import re
import sys

PATCHES = [
    ("0012",
     re.compile(r"(L_088360A4:\n(?:    psprecomp_profile_label = 0x088360A4u;\n)?)"),
     r"\1    ctx.gpr[5] = burnout_frame_rate(ctx.gpr[5]);  // patch_frame_rate.py\n",
     "std::uint32_t burnout_frame_rate(std::uint32_t half_rate);  // patch_frame_rate.py\n"),
    ("0020",
     re.compile(r"(L_088553F4:\n(?:    psprecomp_profile_label = 0x088553F4u;\n)?    ctx\.gpr\[4\] = \(0u \| 2u\);\n)"),
     r"\1    ctx.gpr[4] = burnout_vblanks_per_frame(rt.memory(), ctx.gpr[16], ctx.gpr[4]);  // patch_frame_rate.py\n",
     "std::uint32_t burnout_vblanks_per_frame(psprecomp::GuestMemory &memory, std::uint32_t display, "
     "std::uint32_t vblanks);  // patch_frame_rate.py\n"),
    ("0002",
     re.compile(r"(L_0880F48C:\n(?:    psprecomp_profile_label = 0x0880F48Cu;\n)?(?:.*\n){2})"
                # PC corpus: aot_store32(base + offset, value); Vita corpus: aot_store32_at(base, offset, value).
                r"(    aot_mem\.aot_store32(?:\(ctx\.gpr\[4\] \+ static_cast<std::uint32_t>\(12204\)|_at\(ctx\.gpr\[4\], 12204), ctx\.gpr\[2\]\);\n)"),
     r"\1    ctx.gpr[2] = burnout_frame_steps(rt.memory(), ctx.gpr[2]);  // patch_frame_rate.py\n\2",
     "std::uint32_t burnout_frame_steps(psprecomp::GuestMemory &memory, std::uint32_t steps);  // patch_frame_rate.py\n"),
]


def main() -> int:
    for unit, pattern, hooked, declaration in PATCHES:
        path = pathlib.Path(sys.argv[1]) / f"generated_unit_{unit}.cpp"
        text = path.read_text(encoding="utf-8")
        if declaration in text:
            print("already patched", path)
            continue
        text, found = pattern.subn(hooked, text)
        if found != 1:
            print("not found in", path)
            return 1
        marker = "namespace psprecomp {\n"
        at = text.find(marker)
        if at < 0:
            print("namespace not found")
            return 1
        text = text[:at] + declaration + text[at:]
        path.write_bytes(text.encode("utf-8"))
        print("patched", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
