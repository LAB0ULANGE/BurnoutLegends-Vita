"""Burnout Legends (ULES00125): hot game functions rewritten as host code
(host/native_funcs.cpp). At each function's first block a call to the host
version returns to the caller when it did the work; otherwise the translated
code runs. The function's last block reports its result (PC check mode).

usage: python patch_native.py GENERATED_DIR   (patches the units in place)
"""
import pathlib
import re
import sys

RETURN = """        jump_target = ctx.gpr[31];
        local_pc = jump_target;
        if (++local_transfers < 2048u) { entry_id = 0u; goto LOCAL_DISPATCH; }
        ctx.pc = jump_target;
        return;
"""

# (unit, entry label, unit of the end label, end label)
FUNCTIONS = [
    ("0142", "08A3FCD4", "0143", "08A401F0"),
]

# 64-bit results (v0, v1): (unit, entry label, label of the block that
# returns; the end hook goes just before its jump to ra, after v0/v1 are set).
FUNCTIONS64 = [
    ("0165", "08A99388", "08A994F0"),  # __udivdi3 (2026-11-13, ~2.7 % of the game code)
    ("0165", "08A998FC", "08A99A78"),  # __umoddi3 (~2.8 %)
]


def label_re(address: str) -> re.Pattern:
    # A profiling corpus has a label store after the label.
    return re.compile(r"(L_%s:\n(?:    psprecomp_profile_label = 0x%su;\n)?)" % (address, address))


def declare(text: str, line: str) -> str:
    if line in text:
        return text
    marker = "namespace psprecomp {\n"
    at = text.find(marker)
    if at < 0:
        raise SystemExit("namespace not found")
    return text[:at] + line + text[at:]


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    for unit, entry, end_unit, end in FUNCTIONS:
        name = f"burnout_native_{entry}"
        path = root / f"generated_unit_{unit}.cpp"
        text = path.read_text(encoding="utf-8")
        if f"{name}(rt.memory(), ctx)" not in text:
            hook = (r"\1    if (%s(rt.memory(), ctx)) {  // patch_native.py\n" % name) + RETURN.replace("\\", "\\\\") + "    }\n"
            text, found = label_re(entry).subn(hook, text)
            if found != 1:
                raise SystemExit(f"L_{entry} not found once in {path}")
            text = declare(text, f"bool {name}(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx);  // patch_native.py\n")
            path.write_text(text, encoding="utf-8")
            print("patched", path, entry)
        path = root / f"generated_unit_{end_unit}.cpp"
        text = path.read_text(encoding="utf-8")
        if f"{name}_end(ctx.gpr[2])" not in text:
            text, found = label_re(end).subn(r"\1    %s_end(ctx.gpr[2]);  // patch_native.py\n" % name, text)
            if found != 1:
                raise SystemExit(f"L_{end} not found once in {path}")
            text = declare(text, f"void {name}_end(std::uint32_t v0);  // patch_native.py\n")
            path.write_text(text, encoding="utf-8")
            print("patched", path, end)
    for unit, entry, end in FUNCTIONS64:
        name = f"burnout_native_{entry}"
        path = root / f"generated_unit_{unit}.cpp"
        text = path.read_text(encoding="utf-8")
        if f"{name}(rt.memory(), ctx)" in text:
            continue
        hook = (r"\1    if (%s(rt.memory(), ctx)) {  // patch_native.py\n" % name) + RETURN.replace("\\", "\\\\") + "    }\n"
        text, found = label_re(entry).subn(hook, text)
        if found != 1:
            raise SystemExit(f"L_{entry} not found once in {path}")
        m = label_re(end).search(text)
        if m is None:
            raise SystemExit(f"L_{end} not found in {path}")
        jump = text.find("    jump_target = ctx.gpr[31];\n", m.end())
        if jump < 0 or text.find("\nL_", m.end()) < jump:
            raise SystemExit(f"no return in the block L_{end}")
        text = text[:jump] + f"    {name}_end(ctx.gpr[2], ctx.gpr[3]);  // patch_native.py\n" + text[jump:]
        text = declare(text, f"bool {name}(psprecomp::GuestMemory &memory, psprecomp::AllegrexContext &ctx);  // patch_native.py\n")
        text = declare(text, f"void {name}_end(std::uint32_t v0, std::uint32_t v1);  // patch_native.py\n")
        path.write_text(text, encoding="utf-8")
        print("patched", path, entry, end)
    return 0


if __name__ == "__main__":
    sys.exit(main())
