# The driver crash, and what causes it

`CreateStateObject` access-violates inside NVIDIA's Pascal driver. It is
**intermittent**, roughly 40 to 80 percent per COLD compile, so a single clean
run proves nothing. Clear `%LOCALAPPDATA%\NVIDIA\DXCache` before every trial
and take the result over at least 15 of them.

## The cause

**A hit shader reading the cbuffer bound through the LOCAL ROOT SIGNATURE**,
`cb0, space1`, which is how this shim delivers `GeometryIndex` and
`InstanceContributionToHitGroupIndex` as shader-record root constants.

Creating and associating the local root signature is fine. Reading from it in
an any-hit or a closest-hit is what removes the device.

## The files

    crash.*      a generated library that reads the record constants. CRASHES.
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

Not the parameter type, not the register space. Reading a cbuffer bound
through the local root signature from a hit shader is the whole of it.

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
