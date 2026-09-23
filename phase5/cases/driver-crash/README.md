# The driver crash, and what causes it

`CreateStateObject` access-violates inside NVIDIA's Pascal driver. It is
**intermittent**, roughly 40 to 80 percent per COLD compile, so a single clean
run proves nothing. Clear `%LOCALAPPDATA%\NVIDIA\DXCache` before every trial
and take the result over at least 15 of them.

## The cause, corrected in 0.40.0

**An UNANNOTATED resource handle, bound through the LOCAL ROOT SIGNATURE, in a
Shader Model 6.6 library.** At 6.6 DXC follows every `createHandleForLib` with
an `annotateHandle` saying what the resource is. This project's lowering did
not, for the one resource it adds itself, the shader record's data. A hit
shader reaching that resource through a bare `createHandleForLib` crashes the
driver inside `CreateStateObject` on about half of all cold compiles.

Measured one variable at a time on the same Unreal shader, cache cleared
before every trial, each pair interleaved in one session:

    record as a CBUFFER (local root constants)
      unannotated, crash.out.dxil              9 of 15 SEGFAULT
      annotated,   cbann.out.dxil              0 of 15
    record as a RAW BUFFER (local root SRV)
      unannotated, crash_srv.out.dxil         10 of 15 SEGFAULT
      annotated,   crash_ann.out.dxil          0 of 15
    the 0.40.0 rewriter, crash_040.out.dxil    0 of 15
    60 record-reading Unreal libraries from one Escher run, lowered
    by the 0.40.0 rewriter, one cold compile each                0 of 60

So it was never the cbuffer, and never reading the local root signature as
such. The conclusion below this section, "reading a cbuffer bound through the
local root signature is the whole of it", was wrong, and 0.37.0 to 0.39.x
baked the values into copies of the hit shaders to avoid a read that only
needed annotating. The earlier controls are all consistent with this: the
read-free variants had no unannotated local handle left, and `globalrec`,
which kept the bare handle but bound it GLOBALLY, was clean, so the local
binding is part of the condition.

**How the wrong answer was reached, which is the part worth keeping.** Every
variant in the table further down changed how the record was DELIVERED, and
none changed how its HANDLE was formed, so the handle form was never tested.
The first local root SRV variant, `localsrv`, was then made in DXC's exact 6.6
shape, which annotates, and it was 0 of 30. That was read as "a root SRV is
safe" when the variant had changed TWO things. Only the rewriter's own
unannotated SRV output crashing, 7 of 15, exposed it. A variant has to change
one thing, and a hand-made one copied from DXC changes whatever DXC does
differently too.

`make_localsrv.py` and `make_annotated.py` are the edits, as scripts.

## The files

    crash.*      a generated library that reads the record constants through
                 an unannotated handle. CRASHES.
    cbann.*      the same, with ONLY the cbuffer handle annotated. CLEAN.
    crash_srv.*  the shader through a 0.40.0 rewriter that read a raw buffer
                 through an unannotated handle. CRASHES.
    crash_ann.*  crash_srv with ONLY the handle annotated. CLEAN.
    crash_040.*  the shader through the 0.40.0 rewriter as shipped. CLEAN.
    localsrv.*   crash.* hand-edited to DXC's annotated raw buffer shape; it
                 changed two things, see above. CLEAN.
    norecread.*  the same library with only the READS removed. The local root
                 signature is still built and still associated. CLEAN.
    globalrec.*  the same library with the same downstream code and the same
                 dynamic values, read from a GLOBAL cbuffer instead. CLEAN.

`globalrec` is the control that carries the result: identical arithmetic,
identical `rawBufferLoad` chain, values still dynamic, one difference only.

## Running it

    phase5out\sotest.exe crash.out.dxil crash.rs.bin hw

`sotest` finds the shape file by `rfind(".out.dxil")`, so **the library must be
named `<name>.out.dxil`** with `<name>.shape.txt` beside it. Named anything
else it refuses before `CreateStateObject` and exits 1, which looks like a
failure and is not one. A whole session of bisecting was lost to exactly that.
A real crash is a segmentation fault, exit 139, after the line
`calling CreateStateObject with 9 subobjects...`.

## Measured

    crash / lowered_010  untouched                       21 of 29 SEGFAULT
    record read in the any-hit only                       6 of 15
    record read in the closest-hit only                   7 of 15
    globalrec                                             0 of 15
    norecread                                             0 of 10
    the same edit applied to lowered_013                  0 of 15
    lowered_013 untouched                                 8 of 15

Across all nineteen libraries from one Escher run, the four with
`recordconstants=0` are 0 of 80 combined; every one of the fifteen with
`recordconstants=1` crashes given enough trials.

## The two cheap escapes, both dead

`sotest` takes `SOTEST_LOCAL_CBV=1` (a root CBV descriptor instead of root
constants) and `SOTEST_LOCAL_SPACE=N` (a different register space). The
library is untouched by either, so each is a one-variable test.

    root constants, space 1    var_base 11/20, lowered_013 9/20, 015 9/20
    root CBV,       space 1    var_base 13/20, lowered_013 11/20, 015 10/20
    root constants, space 2    var_base 15/20

Not the parameter type, not the register space. **The conclusion drawn here,
"reading a cbuffer bound through the local root signature from a hit shader is
the whole of it", is WRONG**: every one of these kept the handle unannotated.
See the corrected cause at the top.

## The fix shape, measured

`SOTEST_HITGROUPS=N` builds N hit groups from `AnyHit_i` / `ClosestHit_i` and
skips the local root signature entirely. That is the shape the baked-constants
fix would produce, with a distinct immediate per copy so nothing folds.

    SOTEST_HITGROUPS=27 phase5out\sotest.exe bake27.out.dxil bake27.rs.bin hw
    SOTEST_HITGROUPS=64 phase5out\sotest.exe bake64.out.dxil bake64.rs.bin hw

    N=27   61856 bytes, 34 subobjects   2285 ms cold   0 of 20
    N=64  107388 bytes, 71 subobjects   3730 ms cold   0 of 15

27 is what a real Escher scene asked for. No ceiling reached; the cost is
roughly linear, about 34 ms per extra pair of hit shaders on a cold compile.

## The workaround, built in 0.37.0 and REMOVED in 0.40.0

It worked, and it could not scale: a copy of the hit shaders per pair, about
34 ms of driver compile each, where Escher's open world has about 2000
records. 0.40.0 reads each record's pair through an annotated local root SRV
instead, with no copies and no cap.

The record constants are baked into one copy of the hit shaders per
(geometry, contribution) pair, and the local root signature is gone. See
`proxy/rewriter/rq_bake.h`.

    phase5out\sotest.exe crash_baked.out.dxil crash_baked.rs.bin hw

`crash_baked` is `crash.in.dxil` through `dxrw rewrite`, which bakes the pair
(0, 0) exactly as the shim does at pipeline creation.

    crash.out.dxil         as it always was        10 of 15 SEGFAULT
    crash_baked.out.dxil   the same shader, baked   0 of 15

Measured in one session, cache cleared before every trial. Across the fifteen
Unreal libraries from one Escher run that read record constants, baked: 0 of
225. One of them baked with 27 pairs, what Escher's scene asked for: 0 of 15.
