# vitaGL patches (Burnout)

`burnout-vitagl.patch`: our changes to vitaGL (tools/src/vitaGL, upstream
commit 2bdbe89), built into the vitasdk's `libvitaGL.a`:

- scene valid region and depth keep per framebuffer (render scale, 2026-11-07);
- `vglEnableDrawProfile` / `vglGetDrawProfile`: time of each part of
  `glDrawElements` with custom shaders (PERF builds, 2026-11-13);
- GXM state cache (`vglEnableStateCache`, on by default, 2026-11-13): the
  custom-shader draw does not re-patch the vertex program for the same
  program and attribute layout, nor set again the vertex/fragment program or a
  fragment texture equal to the previous draw's; every new scene and every
  other vitaGL path that sets them forgets the cache. Library before it:
  `libvitaGL.a.before_statecache`.

Rebuild: compile the changed sources with the flags of `obj/cmds.sh` and
`arm-vita-eabi-ar r` them into `libvitaGL.a` (the 2026-11-07 library is kept
as `libvitaGL.a.orig_2026-11-07`); copy `source/vitaGL.h` to the vitasdk's
include folder.
