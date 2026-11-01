#!/usr/bin/env python3
"""Changes to the ReXGlue codegen itself for Saints Row and Saints Row 2 (its output's inputs: in
build.sh's codegen key).

    sr_codegen.py <gta4-ps5 checkout>

- dcbz clears the Xbox 360's whole 128-byte cache line, not 32 bytes: the Xenon has no short
  line. Xenia Canary 72ce130 ("Xbox360 cache line is always 128 bytes") is what stopped Alex
  falling through the world in Saints Row (xenia-canary/game-compatibility#501): Titanium's
  memory clearing counts on the 128.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1]) / "glue/rexglue-sdk-main"
p = root / "src/codegen/builders/system.cpp"
s = p.read_text()
old = r'''bool build_dcbz(BuilderContext& ctx) {
  // Compute EA, align to 32-byte cache line, apply physical offset
  ctx.print("\t{} = (", ctx.ea());
  if (ctx.insn.operands[0] != 0)
    ctx.print("{}.u32 + ", ctx.r(ctx.insn.operands[0]));
  ctx.println("{}.u32) & ~31;", ctx.r(ctx.insn.operands[1]));
  ctx.println("\tmemset((void*)REX_RAW_ADDR({}), 0, 32);", ctx.ea());
  return true;
}'''
new = r'''bool build_dcbz(BuilderContext& ctx) {
  // [sr] The Xbox 360's cache line is always 128 bytes (Xenia Canary 72ce130): dcbz clears
  // all of it, as dcbz128 does
  return build_dcbzl(ctx);
}'''
if s.count(old) != 1:
    sys.exit("sr_codegen.py: build_dcbz isn't as expected")
s = s.replace(old, new)
s = s.replace("bool build_dcbz(BuilderContext& ctx) {", "bool build_dcbzl(BuilderContext& ctx);\n\nbool build_dcbz(BuilderContext& ctx) {", 1)
p.write_text(s)
print("sr_codegen.py: dcbz clears 128 bytes")
