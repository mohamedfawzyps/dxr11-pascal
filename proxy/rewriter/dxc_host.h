// Hosting DXC so the rewriter can be handed what the proxy actually receives.
//
// The rewriter works on .ll TEXT; D3D12 hands us a DXIL CONTAINER. Going from
// one to the other and back needs DXC:
//
//   container -> text     IDxcCompiler::Disassemble   (dxcompiler.dll)
//   text -> container     IDxcAssembler               (dxcompiler.dll)
//   validate and sign     IDxcValidator               (dxil.dll)
//
// Phase 1 established that dxil.dll's validator signs anything that validates,
// with no secret key, so the last step is the same mechanism used throughout.
//
// This is the runtime dependency the fork accepted: the shim has to ship these
// two DLLs. Three consequences are handled here rather than discovered later.
//
// 1. LOAD BY FULL PATH, NOT BY NAME. An application may already have its own
//    dxcompiler.dll loaded, and LoadLibraryW(L"dxcompiler.dll") would return
//    THEIRS, of some other version. The shim loads the copies sitting beside
//    itself, found from its own module handle.
// 2. NEVER THROW, NEVER CRASH. This runs inside someone else's process. Every
//    failure is a returned error, and a missing DLL is a clear message rather
//    than a fault.
// 3. LOAD LAZILY AND ONCE. Nothing is loaded until a shader actually needs
//    rewriting, so applications that never use RayQuery pay nothing.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace dxch {

// Remember the proxy's own module so the DLLs beside it can be found. Called
// from DllMain. Optional: without it, the directory of the current module is
// used, which is the same thing in practice but only if this code is in the
// proxy rather than a test exe.
void SetHostModule(HMODULE self);

// Is DXC available? Loads on first call. `error` says why not, when false.
bool Available(std::string* error);

// Which DXC was actually loaded, for the log, e.g.
// "dxcompiler 1.10.2605.37, dxil 1.10.2605.37".
//
// The first question about any signing or validation failure is which compiler
// produced it, and a proxy DLL is copied next to an executable and then
// forgotten. Returns "not loaded" before the first lowering, since DXC loads
// lazily.
const char* Versions();

// A DXIL container to its .ll disassembly.
bool Disassemble(const void* container, size_t size, std::string* text,
                 std::string* error);

// .ll text to a DXIL container, validated and SIGNED. The container comes back
// in `out`, ready to hand to D3D12.
bool AssembleAndSign(const std::string& text, std::vector<uint8_t>* out,
                     std::string* error);

// The whole round trip for one shader: disassemble, hand the text to `xform`,
// then assemble and sign the result. `xform` returns false to refuse, in which
// case nothing is produced and its reason is passed back.
//
// Refusal is not failure: a shader the rewriter declines is forwarded to the
// driver unchanged by the caller.
bool RewriteContainer(const void* container, size_t size,
                      bool (*xform)(const std::string& in, std::string* out,
                                    std::string* why, void* ctx),
                      void* ctx, std::vector<uint8_t>* out, std::string* error);

}  // namespace dxch
