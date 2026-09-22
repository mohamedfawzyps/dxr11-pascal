# An offline reproduction of the CreateStateObject driver crash

`crash.out.dxil` is a library this project GENERATED from
`VolumeHardwareRayTraceLightSamplesCS`, a real UE 5.8.2 compute shader dumped
out of a running game. Built with its own root signature on a GTX 1070, on a
fresh device, in a process that does nothing else:

    phase5out\sotest.exe crash.out.dxil crash.rs.bin hw

the NVIDIA driver access-violates inside `CreateStateObject`. One second, no
game, no Unreal, no other state object, 28036 bytes of library.

This is the thing eleven versions of bisecting could not reach. Every earlier
attempt to replay the failure offline SUCCEEDED, which kept pointing at the
process context and was a dead end; it only looked that way because the one
shader being replayed, `RayTracingDebugMainCS`, happens to be survivable on a
quiet device and is not survivable in the game. This one is not survivable
anywhere.

- `crash.in.dxil`   the Unreal shader, unmodified, as the game handed it over
- `crash.out.dxil`  what the rewriter made of it: signed, validated, accepted
                    by D3D12, and fatal to the driver
- `crash.rs.bin`    the application's compute root signature, which becomes the
                    state object's GLOBAL root signature
- `crash.shape.txt` what the shim decided, so sotest builds the same subobjects

D3D12's own validation has nothing to say about it. With the debug layer
forced on, the only message at the moment of death is the device removal
itself.

What this buys is ITERATION. Cutting the library down, changing one construct
at a time and asking which one the driver cannot compile is now a one second
experiment instead of a game launch.
