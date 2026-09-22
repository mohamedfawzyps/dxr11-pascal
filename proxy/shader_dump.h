// Writing out the shaders the rewriter refused, so they can be looked at.
//
// The project has been working blind on the last refusals. "2 concurrent
// RayQuery objects in LumenScreenProbeGatherHardwareRayTracingCS" says what the
// check decided, not whether it was right, and the check counts allocateRayQuery
// CALLS rather than whether two queries are ever live at once. Two queries used
// one after the other are refused identically. Deciding that needs the actual
// DXIL, and the only place it exists is inside the game's process.
//
// So: dump the container. `dxrw rewrite` takes one and reproduces the exact
// failure offline, which turns "33 shaders refused" into 33 files that can be
// disassembled and read.
//
// OFF by default, and bounded. This writes megabytes into somebody's folder,
// so it happens only when asked and stops after a fixed number of files.
#pragma once

#include <windows.h>

#include <cstddef>

namespace shdump {

// Write one refused shader and its reason, if dumping is on. Safe to call from
// several threads: Unreal creates pipelines on a pool.
//
// Does nothing at all when the `dump` setting is empty, which is the default,
// including reading the setting more than once.
void Refused(const void* container, size_t size, const char* why);

// Write one shader that DID lower, as a pair: the application's container and
// the library generated from it.
//
// Refusals were the only thing dumped, which left the successes unreachable.
// A real Unreal session lowered four shaders, built their state objects, and
// then the driver died with DXGI_ERROR_DRIVER_INTERNAL_ERROR. Those four were
// the prime suspects and none of them existed anywhere on disk, so there was
// nothing to replay through CreateStateObject without launching the game
// again.
//
// Same setting, same cap, separate counter, because a run that lowers a lot
// and refuses a little should not lose its refusals to the limit.
void Lowered(const void* original, size_t originalSize,
             const void* lowered, size_t loweredSize);

}  // namespace shdump
