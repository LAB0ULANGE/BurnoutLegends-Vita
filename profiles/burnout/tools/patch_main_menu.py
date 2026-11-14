"""Burnout Legends (ULES00125): the main menu (WORLD TOUR, SINGLE EVENT,
MULTIPLAYER, DRIVER DETAILS) is built by 0x088B7004, which writes its row
count (4) at 0x088B70FC/0x088B7104 just before it builds the rows on screen.
This post-pass asks the host instead (burnout_main_menu_rows, bonus_menu.cpp),
which answers 5 and fills the fifth row (BONUS) when the Bonus menu is on, so
the game builds its menu with that row on screen.

usage: python patch_main_menu.py GENERATED_DIR   (patches the unit holding 0x088B70F8 in place)
"""
import pathlib
import re
import sys

# The block (a profiling corpus has a label store after the label).
ROWS = re.compile(r"(L_088B70F8:\n(?:    psprecomp_profile_label = 0x088B70F8u;\n)?"
                  r"    ctx\.gpr\[4\] = \(aot_mem\.aot_load32(?:_at\(ctx\.gpr\[16\], 4\)|\(ctx\.gpr\[16\] \+ static_cast<std::uint32_t>\(4\)\))\);\n)"
                  r"    ctx\.gpr\[5\] = \(0u \| 4u\);\n")
ROWS_HOOKED = r"\1    ctx.gpr[5] = burnout_main_menu_rows(rt.memory(), ctx.gpr[16]);  // patch_main_menu.py (was 4)\n"
DECLARATION = "std::uint32_t burnout_main_menu_rows(psprecomp::GuestMemory &memory, std::uint32_t menu);  // patch_main_menu.py\n"


def main() -> int:
    units = [u for u in sorted(pathlib.Path(sys.argv[1]).glob("generated_unit_*.cpp")) if "L_088B70F8:" in u.read_text(encoding="utf-8")]
    if len(units) != 1:
        print("unit with 0x088B70F8 not found")
        return 1
    path = units[0]
    text = path.read_text(encoding="utf-8")
    if "burnout_main_menu_rows" in text:
        print("already patched")
        return 0
    text, found = ROWS.subn(ROWS_HOOKED, text)
    if found != 1:
        print("main menu row count not found")
        return 1
    # Declared at namespace scope, before the first function of the unit.
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
