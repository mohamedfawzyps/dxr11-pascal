// Reading the version of the DLLs this shim depends on, or competes with.
//
// Four DLLs decide whether a run works, and none of them are ours:
//
//   dxcompiler.dll      converts a DXIL container to text and back
//   dxil.dll            signs the result
//   D3D12Core.dll       the application's Agility SDK runtime, if it ships one
//   d3d12SDKLayers.dll  the debug layer, when it is on
//
// A proxy DLL gets copied next to an executable and then forgotten, so the
// first question about any report is which copies were actually in play. That
// has to come from the running process rather than from what someone remembers
// installing: an application with its own Agility SDK redirects D3D12Core.dll
// to a subdirectory, and an application with its own dxcompiler.dll has one
// loaded from somewhere else entirely.
//
// Never throws, never fails: a DLL that is absent reports "not loaded", which
// is itself the answer to the question.
#pragma once

#include <windows.h>

#include <string>

namespace dllinfo {

// The file version resource as "a.b.c.d", or "unknown" when the file has none.
// Taken from the file rather than from any API the DLL offers, so it works the
// same for all four and reports the build number a bug report needs.
std::string OfFile(const std::wstring& path);

// A module by handle. Returns "version (full path)", or "not loaded" for null.
std::string OfModule(HMODULE h);

// A module ALREADY LOADED in this process, named as the loader knows it
// ("D3D12Core.dll"). Returns "version (full path)", or "not loaded".
//
// Deliberately does NOT load anything. Asking whether D3D12Core.dll is present
// must not be what causes it to be present.
//
// Matches on the BASE NAME, so inside a proxy called d3d12.dll, asking for
// "d3d12.dll" answers with the proxy. Use OfModule with the real handle there.
std::string OfLoadedModule(const wchar_t* name);

}  // namespace dllinfo
