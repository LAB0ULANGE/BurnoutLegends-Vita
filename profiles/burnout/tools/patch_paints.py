"""Burnout Legends (ULES00125): COLOR in car select (0x088F87E0) sets the car's
colour to (colour + 1) modulo the colours it offers. This post-pass lets the
host (burnout_car_colour, paints.cpp) take the result: after the game's last
colour come the extra paints the player unlocked (shown on the first colour).

usage: python patch_paints.py GENERATED_DIR   (patches generated_unit_0061.cpp in place)
"""
import pathlib
import re
import sys

NEXT = re.compile(r"(L_088F882C:\n(?:.*\n){5,10}?.*ctx\.hi = static_cast<std::uint32_t>\(dividend % divisor\); \} \}\n)"
                  r"    ctx\.gpr\[5\] = \(ctx\.hi\);\n"
                  r"(    ctx\.gpr\[31\] = \(0x088F8858u\);\n)")
HOOKED = (r"\1    ctx.gpr[5] = burnout_car_colour(rt.memory(), ctx.gpr[6], ctx.gpr[5], ctx.hi);  // patch_paints.py\n\2")
DECLARATION = ("std::uint32_t burnout_car_colour(psprecomp::GuestMemory &memory, std::uint32_t next, std::uint32_t count, "
               "std::uint32_t native);  // patch_paints.py\n")


def main() -> int:
    path = pathlib.Path(sys.argv[1]) / "generated_unit_0061.cpp"
    text = path.read_text(encoding="utf-8")
    if "burnout_car_colour" in text:
        print("already patched")
        return 0
    text, found = NEXT.subn(HOOKED, text)
    if found != 1:
        print("COLOR press not found")
        return 1
    marker = "namespace psprecomp {\n"
    at = text.find(marker)
    if at < 0:
        print("namespace not found")
        return 1
    text = text[:at] + DECLARATION + text[at:]
    path.write_text(text, encoding="utf-8")
    print("patched", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
