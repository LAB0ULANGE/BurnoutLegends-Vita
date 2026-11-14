"""Burnout Legends (ULES00125): the game records a Signature Takedown in the
profile at the end of a race (0x08817050: profile[1158 + i] = 1). This
post-pass calls the host first (burnout_signature_done, bonus_menu.cpp), which
gives that Signature's Collector car right then, so the game's own results
see the new car and announce it ("NEW CAR UNLOCKED").

usage: python patch_signatures.py GENERATED_DIR   (patches generated_unit_0004.cpp in place)
"""
import pathlib
import re
import sys

SETTER = re.compile(r"(L_08817050:\n(?:    psprecomp_profile_label = 0x08817050u;\n)?)"
                    r"(    ctx\.gpr\[6\] = \(0u \| 1u\);\n"
                    r"    ctx\.gpr\[4\] = \(ctx\.gpr\[4\] \+ ctx\.gpr\[5\]\);\n)")
HOOKED = r"\1    burnout_signature_done(rt.memory(), ctx.gpr[4], ctx.gpr[5]);  // patch_signatures.py\n\2"
DECLARATION = ("void burnout_signature_done(psprecomp::GuestMemory &memory, std::uint32_t profile, "
               "std::uint32_t signature);  // patch_signatures.py\n")


def main() -> int:
    path = pathlib.Path(sys.argv[1]) / "generated_unit_0004.cpp"
    text = path.read_text(encoding="utf-8")
    if "burnout_signature_done" in text:
        print("already patched")
        return 0
    text, found = SETTER.subn(HOOKED, text)
    if found != 1:
        print("signature setter not found")
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
