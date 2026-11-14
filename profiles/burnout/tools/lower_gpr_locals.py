"""Experiment (2026-11-13 night): guest GPRs 1-31 as C++ locals inside a
generated unit's entry function, written back to the context before every
return and every call that receives the context, and read again after it.

--fpr: the 32 FPU registers too (F0-F31; the corpus only reaches them as
ctx.fpr[N], AllegrexContext's fpr_bits/set_fpr_bits are not used).

usage: python lower_gpr_locals.py IN.cpp OUT.cpp [--fpr]
"""
import re
import sys

with_fpr = "--fpr" in sys.argv[3:]
src = open(sys.argv[1], encoding="utf-8").read()
start = re.search(r"^void (recomp_unit_\d+)_entry\(Runtime &rt, AllegrexContext &ctx,.*\{\n", src, re.M)
name = start.group(1)
end = src.index("\nvoid %s(Runtime &rt, AllegrexContext &ctx) {" % name)
head, body, tail = src[:start.end()], src[start.end():end], src[end:]

regs = range(1, 32)
flush = " ".join("ctx.gpr[%d] = G%d;" % (r, r) for r in regs)
reload = " ".join("G%d = ctx.gpr[%d];" % (r, r) for r in regs)
decl = "    std::uint32_t " + ", ".join("G%d = ctx.gpr[%d]" % (r, r) for r in regs) + ";\n"
if with_fpr:
    if re.search(r"ctx\.(fpr_bits|set_fpr_bits)\(|ctx\.fpr\.", body):
        raise SystemExit("FPU registers reached otherwise than ctx.fpr[N]")
    fregs = range(32)
    flush += " " + " ".join("ctx.fpr[%d] = F%d;" % (r, r) for r in fregs)
    reload += " " + " ".join("F%d = ctx.fpr[%d];" % (r, r) for r in fregs)
    decl += "    float " + ", ".join("F%d = ctx.fpr[%d]" % (r, r) for r in fregs) + ";\n"
    body = re.sub(r"ctx\.fpr\[(\d+)\]", r"F\1", body)

# Registers -> locals (the context's own fields stay for flush/reload, added after).
body = re.sub(r"ctx\.gpr\[(\d+)\]", lambda m: "G" + m.group(1) if m.group(1) != "0" else "0u", body)
body = re.sub(r"ctx\.set_gpr\((\d+), ", lambda m: "SETG%s(" % m.group(1), body)


# ADD/SUB with overflow trap: AllegrexContext::execute_signed_add/sub read and
# write ctx.gpr themselves, so they are done here on the locals instead.
def signed_op(m):
    op, d, a, b = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))
    reg = lambda r: "G%d" % r if r != 0 else "0u"
    sign = "+" if op == "add" else "-"
    write = "G%d = static_cast<std::uint32_t>(static_cast<std::int32_t>(r)); " % d if d != 0 else ""
    return ("[&]() -> bool { const std::int64_t r = static_cast<std::int64_t>(static_cast<std::int32_t>(%s)) %s "
            "static_cast<std::int64_t>(static_cast<std::int32_t>(%s)); if (r < INT32_MIN || r > INT32_MAX) return false; "
            "%sreturn true; }()" % (reg(a), sign, reg(b), write))


body = re.sub(r"ctx\.execute_signed_(add|sub)\((\d+)u, (\d+)u, (\d+)u\)", signed_op, body)
if "execute_signed_" in body:
    raise SystemExit("execute_signed_* left with another form")

out = []
calls = 0
for line in body.split("\n"):
    stripped = line.strip()
    # if (CALL(ctx, ...) && ctx.pc == X) goto L;
    m = re.match(r"^(\s*)if \((rt\.invoke_chained_(?:direct<[^>]*>|call)\(ctx, &aot_mem_shared\)) && (ctx\.pc == 0x[0-9A-Fa-f]+u)\) (goto L_[0-9A-F]+;)$", line)
    if m:
        ind, call, cond, goto = m.groups()
        out.append("%s{ %s const bool chained_ok = %s; %s if (chained_ok && %s) %s }" % (ind, flush, call, reload, cond, goto))
        calls += 1
        continue
    m = re.match(r"^(\s*)\(void\)(rt\.invoke_chained_(?:direct<[^>]*>|call)\(ctx, &aot_mem_shared\)); return;$", line)
    if m:
        ind, call = m.groups()
        out.append("%s{ %s (void)%s; RETURN_KEEP }" % (ind, flush, call))
        calls += 1
        continue
    m = re.match(r"^(\s*)\(void\)(rt\.invoke_chained_(?:direct<[^>]*>|call)\(ctx, &aot_mem_shared\));$", line)
    if m:
        ind, call = m.groups()
        out.append("%s{ %s (void)%s; %s }" % (ind, flush, call, reload))
        calls += 1
        continue
    m = re.match(r"^(\s*)if \((burnout_native_[0-9A-F]+\(rt\.memory\(\), ctx\))\) \{(.*)$", line)
    if m:
        ind, call, rest = m.groups()
        # A lambda: no initialised local for the labels' gotos to jump over.
        out.append("%sif ([&] { %s const bool native_ok = %s; %s return native_ok; }()) {%s" % (ind, flush, call, reload, rest))
        calls += 1
        continue
    if re.search(r"\(ctx[,)]|, ctx\)", line):
        raise SystemExit("unhandled call with the context: " + stripped)
    out.append(line)
body = "\n".join(out)
# Every return writes the locals back (after a call they were read again).
body = re.sub(r"\breturn;", "{ " + flush + " return; }", body)
body = body.replace("RETURN_KEEP", "return;")  # after a call: the context is already current
# SETGn(value) -> Gn = (value)
body = re.sub(r"SETG(\d+)\(", r"G\1 = (", body)

marker = re.search(r"    std::uint32_t entry_id = [^\n]*\n", body)
body = body[:marker.end()] + decl + body[marker.end():]
open(sys.argv[2], "w", encoding="utf-8", newline="\n").write(head + body + tail)
print(name, "calls rewritten:", calls)
