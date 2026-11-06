"""Burnout Legends (ULES00125): the game's libc memcpy (0x08A7F504) and memset
(0x08A7F5D8) copy and fill one byte per loop iteration. This post-pass puts a
host memmove/memset in front of each loop block: it does every remaining
iteration at once and leaves the registers as the loop leaves them, and falls
back to the translated loop when the copy overlaps forwards (dst inside
(src, src + n)) or the range is not one contiguous host block.

usage: python patch_libc_loops.py GENERATED_DIR   (patches generated_unit_0158.cpp in place)
"""
import pathlib
import sys

MEMCPY_LOOP = "L_08A7F51C:\n"
MEMCPY_NATIVE = """L_08A7F51C:
    { // memcpy loop (patch_libc_loops.py): byte n-1-gpr4.. copied forward, gpr4 = gpr6 = iterations left - 1
      const std::uint32_t n = ctx.gpr[4] + 1u, src = ctx.gpr[5], dst = ctx.gpr[7];
      if (n != 0u && n <= 0x01000000u && dst - src >= n) {
        std::uint8_t *d = rt.memory().raw_pointer(dst, n);
        const std::uint8_t *s = rt.memory().raw_pointer(src, n);
        if (d != nullptr && s != nullptr) {
          std::memmove(d, s, n);
          ctx.gpr[5] = src + n; ctx.gpr[7] = dst + n; ctx.gpr[8] = 0u; ctx.gpr[4] = 0xFFFFFFFFu; ctx.gpr[6] = 0xFFFFFFFFu;
          goto L_08A7F53C;
        }
      } }
    goto L_08A7F51C_loop;
L_08A7F51C_loop:
"""
MEMSET_LOOP = "L_08A7F5EC:\n"
MEMSET_NATIVE = """L_08A7F5EC:
    { // memset loop (patch_libc_loops.py): gpr7 = gpr6 = iterations left - 1, byte gpr5 at gpr8
      const std::uint32_t n = ctx.gpr[7] + 1u, dst = ctx.gpr[8];
      if (n != 0u && n <= 0x01000000u) {
        std::uint8_t *d = rt.memory().raw_pointer(dst, n);
        if (d != nullptr) {
          std::memset(d, static_cast<int>(ctx.gpr[5] & 0xFFu), n);
          ctx.gpr[8] = dst + n; ctx.gpr[9] = 0u; ctx.gpr[7] = 0xFFFFFFFFu; ctx.gpr[6] = 0xFFFFFFFFu;
          goto L_08A7F604;
        }
      } }
    goto L_08A7F5EC_loop;
L_08A7F5EC_loop:
"""

def main() -> int:
    path = pathlib.Path(sys.argv[1]) / "generated_unit_0158.cpp"
    text = path.read_text(encoding="utf-8")
    if "patch_libc_loops.py" in text:
        print("already patched")
        return 0
    for loop, native, back in ((MEMCPY_LOOP, MEMCPY_NATIVE, "goto L_08A7F51C;"), (MEMSET_LOOP, MEMSET_NATIVE, "goto L_08A7F5EC;")):
        if text.count(loop) != 1:
            raise SystemExit(f"loop label {loop.strip()} not found once")
        start = text.index(loop)
        # The loop's own back edge goes to the plain loop, not the native block again.
        end = text.index("\nL_", start + len(loop))
        body = text[start + len(loop):end].replace(back, back.replace(":", "").replace(";", "_loop;"))
        text = text[:start] + native + body + text[end:]
    if "#include <cstring>" not in text:
        text = text.replace("#include <cstdint>\n", "#include <cstdint>\n#include <cstring>\n", 1)
    path.write_text(text, encoding="utf-8")
    print("patched", path)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
