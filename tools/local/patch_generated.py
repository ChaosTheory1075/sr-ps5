#!/usr/bin/env python3
"""Xenia game-patches that change one instruction, applied to the recompiled code.

    patch_generated.py <game 1|2> <generated dir>

A code patch can't be written into guest memory here (the code is recompiled C++), so the
instruction's line in the generated function is replaced by the same code behind a switch the app
sets (a cvar, read through an extern "C" function in sr_app.cpp). Each edit must match exactly once,
or the build stops: a codegen change shows here, not as a game that silently lost its patch.
"""
import pathlib
import sys

# (game, function, the instruction's line as generated, the line in its place, the switch)
PATCHES = [
    # Saints Row 2 TU3, "Motion Blur Strength Fix" (Tervel, xenia-canary/game-patches 545107FC):
    # 821D4F9C "fsel f1,f12,f0,f13" (the blur's time step: the frame's) becomes the 1/30 s at
    # 82B62370, so the blur is as strong at 60 fps as the game made it at 30.
    (2, "sub_821D4F28",
     "\tctx.f1.f64 = ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64;\n",
     "\tctx.f1.f64 = sr_motion_blur_fix() ? double(1.0f / 30.0f) : (ctx.f12.f64 >= 0.0 ? ctx.f0.f64 : ctx.f13.f64);  // [sr]\n",
     "sr_motion_blur_fix"),
]


def main():
    game = int(sys.argv[1])
    gen = pathlib.Path(sys.argv[2])
    for g, func, old, new, switch in PATCHES:
        if g != game:
            continue
        head = f"DEFINE_REX_FUNC({func})"
        files = [f for f in sorted(gen.glob("*.cpp")) if head in f.read_text()]
        if len(files) != 1:
            sys.exit(f"patch_generated: {func} in {len(files)} files")
        f = files[0]
        text = f.read_text()
        start = text.index(head)
        end = text.index("\n}\n", start)
        body = text[start:end]
        if body.count(old) != 1:
            sys.exit(f"patch_generated: {func}: expected 1 of {old.strip()!r}, found {body.count(old)}")
        body = body.replace(old, new)
        decl = f'extern "C" bool {switch}();  // [sr] (sr_app.cpp)\n'
        text = text[:start] + body + text[end:]
        if decl not in text:
            text = decl + text
        f.write_text(text)
        print(f"patched  {f.name}: {func}")


if __name__ == "__main__":
    main()
