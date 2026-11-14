"""Burnout Legends (ULES00125): the EA Trax settings (PARTOUT / ... / NON) are
21 bytes at 0x08BFCDE0 + 0x7400, other settings right after. Three places
index them with a track number that can pass 21 once tracks are added
(host/trax.cpp, EA_TRAX.md); they go to the host instead (the first 21 as
before, the added ones in our file):

- 0x088C6D44  sb  the menu's commit, every track       -> burnout_trax_store
- 0x088C6DF4  lbu the menu's load, every track         -> burnout_trax_load
- 0x089B0370  lbu / 0x089B0380 sb: the playing track's -> burnout_trax_load / _store
  setting with bit 3 cleared
- 0x089B2E10  lbu 0(s0 + i): the settings applied to     -> burnout_trax_load_at
  every track (s0: the settings, or a copy of them)

usage: python patch_trax.py GENERATED_DIR   (patches the units in place)
"""
import pathlib
import re
import sys

# aot_load8(a + static_cast<std::uint32_t>(29696)) or aot_load8_at(a, 29696)
def load8(reg):
    return (rf"aot_mem\.aot_load8(?:_at\(ctx\.gpr\[{reg}\], 29696\)|\(ctx\.gpr\[{reg}\] \+ static_cast<std::uint32_t>\(29696\)\))")


def store8(reg, value):
    return (rf"aot_mem\.aot_store8(?:_at\(ctx\.gpr\[{reg}\], 29696, |\(ctx\.gpr\[{reg}\] \+ static_cast<std::uint32_t>\(29696\), )"
            rf"static_cast<std::uint8_t>\(ctx\.gpr\[{value}\]\)\);")


def load32(reg, offset):
    return (rf"aot_mem\.aot_load32(?:_at\(ctx\.gpr\[{reg}\], {offset}\)|\(ctx\.gpr\[{reg}\] \+ static_cast<std::uint32_t>\({offset}\)\))")


LABEL = r"(?:    psprecomp_profile_label = 0x[0-9A-F]+u;\n)?"
PATCHES = [
    ("L_088C6D3C:", "trax commit",
     re.compile(r"(L_088C6D3C:\n" + LABEL +
                r"    ctx\.gpr\[4\] = \(" + load32(29, 140) + r"\);\n"
                r"    ctx\.gpr\[5\] = \(ctx\.gpr\[19\] \+ ctx\.gpr\[17\]\);\n)"
                r"    " + store8(5, 4) + r"\n"),
     r"\1    burnout_trax_store(rt.memory(), ctx.gpr[19], ctx.gpr[4]);  // patch_trax.py: trax commit (was sb a0, 0x7400(a1))\n"),
    ("L_088C6DF0:", "trax load",
     re.compile(r"(L_088C6DF0:\n" + LABEL +
                r"    ctx\.gpr\[4\] = \(ctx\.gpr\[18\] \+ ctx\.gpr\[17\]\);\n)"
                r"    ctx\.gpr\[7\] = \(" + load8(4) + r"\);\n"),
     r"\1    ctx.gpr[7] = burnout_trax_load(rt.memory(), ctx.gpr[18]);  // patch_trax.py: trax load (was lbu a3, 0x7400(a0))\n"),
    ("L_089B034C:", "trax playing",
     re.compile(r"(    ctx\.gpr\[6\] = \(ctx\.gpr\[4\] \+ ctx\.gpr\[6\]\);\n)"
                r"    ctx\.gpr\[7\] = \(" + load8(6) + r"\);\n"
                r"(    ctx\.gpr\[4\] = \(ctx\.gpr\[16\] \| 0u\);\n"
                r"    ctx\.gpr\[5\] = \(ctx\.gpr\[7\] & ctx\.gpr\[5\]\);\n"
                r"    ctx\.gpr\[31\] = \(0x089B0384u\);\n)"
                r"    " + store8(6, 5) + r"\n"),
     r"\1    ctx.gpr[7] = burnout_trax_load(rt.memory(), ctx.gpr[6] - 0x08BFCDE0u);  // patch_trax.py: trax playing (was lbu a3, 0x7400(a2))\n"
     r"\2    burnout_trax_store(rt.memory(), ctx.gpr[6] - 0x08BFCDE0u, ctx.gpr[5]);  // patch_trax.py: trax playing (was sb a1, 0x7400(a2))\n"),
    ("L_089B2E0C:", "trax apply",
     re.compile(r"(L_089B2E0C:\n" + LABEL +
                r"    ctx\.gpr\[4\] = \(ctx\.gpr\[17\] \+ ctx\.gpr\[16\]\);\n)"
                r"    ctx\.gpr\[7\] = \(aot_mem\.aot_load8(?:_at\(ctx\.gpr\[4\], 0\)|\(ctx\.gpr\[4\] \+ static_cast<std::uint32_t>\(0\)\))\);\n"),
     r"\1    ctx.gpr[7] = burnout_trax_load_at(rt.memory(), ctx.gpr[16], ctx.gpr[17]);  // patch_trax.py: trax apply (was lbu a3, 0(a0))\n"),
]
DECLARATIONS = ("void burnout_trax_store(psprecomp::GuestMemory &memory, std::uint32_t index, std::uint32_t value);  // patch_trax.py\n"
                "std::uint32_t burnout_trax_load(psprecomp::GuestMemory &memory, std::uint32_t index);  // patch_trax.py\n"
                "std::uint32_t burnout_trax_load_at(psprecomp::GuestMemory &memory, std::uint32_t settings, std::uint32_t index);  // patch_trax.py\n")



def main() -> int:
    folder = pathlib.Path(sys.argv[1])
    for label, name, pattern, replacement in PATCHES:
        units = [u for u in sorted(folder.glob("generated_unit_*.cpp")) if f"{label}\n" in u.read_text(encoding="utf-8")]
        if len(units) != 1:
            print(f"{name}: unit with {label} not found")
            return 1
        path = units[0]
        text = path.read_text(encoding="utf-8")
        if f"patch_trax.py: {name}" in text:
            print(f"{name}: already patched")
            continue
        text, found = pattern.subn(replacement, text)
        if found != 1:
            print(f"{name}: code not found ({found})")
            return 1
        if "std::uint32_t burnout_trax_load_at(" not in text:
            at = text.find("namespace psprecomp {\n")
            if at < 0:
                print("namespace not found")
                return 1
            text = text[:at] + DECLARATIONS + text[at:]
        path.write_text(text, encoding="utf-8")
        print(f"{name}: patched {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
