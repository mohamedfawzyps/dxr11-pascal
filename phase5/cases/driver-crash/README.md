# An offline reproduction of the CreateStateObject driver crash

`crash.out.dxil` is a library this project GENERATED from
`HardwareRayTraceLightSamplesCS`, a real UE 5.8.2 MegaLights compute shader
dumped out of a running game. Built with its own root signature on a GTX 1070:

    phase5out\sotest.exe crash.out.dxil crash.rs.bin hw
    calling CreateStateObject with 9 subobjects...
    Segmentation fault

The NVIDIA driver access-violates INSIDE `CreateStateObject`. No Unreal, no
other state objects, no memory pressure, no concurrency, 35544 bytes of
library. D3D12's own validation has nothing to say about it: with the debug
layer forced on, the only message at the moment of death is the device removal.

## IT IS INTERMITTENT, AND THAT IS THE MOST IMPORTANT THING ON THIS PAGE

It does not fail every time, and it WEARS OFF. Measured on nineteen generated
libraries from one game run, each built alone in its own process:

    first time each was ever compiled   5 of 19 crashed
    sweep again                         1 of 19 crashed, the same one
    sweep again                         1 of 19
    sweep again                         none
    sweep again                         none

The same library that crashed twice then ran ten times in a row without
complaint. So a single clean run proves NOTHING here, and a bisect that trusts
one is worthless. Take the result over several fresh sweeps.

INFERRED, not established: this is the driver's shader disk cache. The crash
looks like it happens while the driver actually COMPILES the library, on a
cache miss, and stops once an entry exists. The obvious test is to clear
`%LOCALAPPDATA%\NVIDIA\DXCache` and try again, which has not been run.

Against that inference: the game crashed on the same shader across many
separate runs, which a warm disk cache should have prevented. So the story is
not complete.

## Files

- `crash.in.dxil`   the Unreal shader, unmodified, as the game handed it over
- `crash.out.dxil`  what the rewriter made of it: signed, validated, accepted
                    by D3D12, and fatal to the driver
- `crash.rs.bin`    the application's compute root signature, which becomes the
                    state object's GLOBAL root signature
- `crash.shape.txt` what the shim decided, so sotest builds the same subobjects

## Why it matters anyway

Every earlier attempt to replay this failure offline SUCCEEDED, which pointed
at the process context and was a dead end. It only looked that way because the
one shader being replayed was `RayTracingDebugMainCS`, which is survivable on
a quiet device. The fault is in something the LIBRARY contains, and iteration
is now seconds rather than a game launch.
