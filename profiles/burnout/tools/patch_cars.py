"""Burnout Legends (ULES00125): car select (object at 0x08D88BF0) takes from
the mode that opens it whether the Race Special class (+769) and the
Heavyweight class (+768, the seventh: the Crash vehicles) are offered; the
right arrow stops before the first class not offered (0x088F8510). This
post-pass asks the host (burnout_heavy_class, bonus_menu.cpp) for the
Heavyweight flag as the screen opens (block 0x088F8C64), so the Bonus can
offer the Heavyweights in Single Event.

usage: python patch_cars.py GENERATED_DIR   (patches generated_unit_0061.cpp in place)
"""
import pathlib
import re
import sys

# The block, in either corpus form (base + offset, or one address).
STORE = re.compile(r"(L_088F8C64:\n(?:.*\n){6,12}?)"
                   r"(    aot_mem\.aot_store8(?:_at\(ctx\.gpr\[19\], 768, |\(ctx\.gpr\[19\] \+ static_cast<std::uint32_t>\(768\), ))")


def hook(match: re.Match) -> str:
    mode = ("aot_mem.aot_load32_at(ctx.gpr[29], 36)" if "_at(" in match.group(2)
            else "aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(36))")
    return (match.group(1) + "    ctx.gpr[4] = burnout_heavy_class(rt.memory(), ctx.gpr[4], ctx.gpr[5], " + mode +
            ");  // patch_cars.py\n" + match.group(2))


DECLARATION = ("std::uint32_t burnout_heavy_class(psprecomp::GuestMemory &memory, std::uint32_t heavy, std::uint32_t special, "
               "std::uint32_t mode);  // patch_cars.py\n")


def main() -> int:
    path = pathlib.Path(sys.argv[1]) / "generated_unit_0061.cpp"
    text = path.read_text(encoding="utf-8")
    if "burnout_heavy_class" in text:
        print("already patched")
        return 0
    text, found = STORE.subn(hook, text)
    if found != 1:
        print("car select class flags not found")
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
