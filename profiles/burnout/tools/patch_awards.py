"""Burnout Legends (ULES00125): the notice of new unlocks (0x089280D4) adds a
"NEW AWARD(S) UNLOCKED" line when its award categories have new items
(their sum, at 0x089281B4). This post-pass adds the host's count
(burnout_new_awards, bonus_menu.cpp: the Bonus filters unlocked since they
were last looked at), so a new filter is announced by the game itself.

usage: python patch_awards.py GENERATED_DIR   (patches generated_unit_0073.cpp in place)
"""
import pathlib
import re
import sys

SUM = re.compile(r"(L_089281B4:\n(?:    psprecomp_profile_label = 0x089281B4u;\n)?"
                 r"    ctx\.gpr\[4\] = \(ctx\.gpr\[17\] \+ ctx\.gpr\[2\]\);\n"
                 r"    ctx\.gpr\[4\] = \(ctx\.gpr\[4\] \+ ctx\.gpr\[20\]\);\n"
                 r"    ctx\.gpr\[4\] = \(ctx\.gpr\[4\] \+ ctx\.gpr\[19\]\);\n"
                 r"    ctx\.gpr\[4\] = \(ctx\.gpr\[4\] \+ ctx\.gpr\[18\]\);\n)")
HOOKED = r"\1    ctx.gpr[4] = (ctx.gpr[4] + burnout_new_awards(rt.memory()));  // patch_awards.py\n"
DECLARATION = "std::uint32_t burnout_new_awards(psprecomp::GuestMemory &memory);  // patch_awards.py\n"


def main() -> int:
    path = pathlib.Path(sys.argv[1]) / "generated_unit_0073.cpp"
    text = path.read_text(encoding="utf-8")
    if "burnout_new_awards" in text:
        print("already patched")
        return 0
    text, found = SUM.subn(HOOKED, text)
    if found != 1:
        print("award count not found")
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
