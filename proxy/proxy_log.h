// Shared logging for the proxies. Writes to %TEMP%\dxr-tier-11-proxy.log, or
// wherever the `log` setting points, and to OutputDebugString.
//
// Definitions live in proxy_log.cpp, which is linked into BOTH d3d12.dll and
// dxgi.dll. ProxyIidName is D3D12's and lives in d3d12_proxy.cpp.
#pragma once

#include <guiddef.h>

// One line, timestamped. Callers supply the trailing newline.
void ProxyLog(const char* fmt, ...);

// The executable we were loaded into, so a log holding several runs says which
// application each one was.
const char* ProxyHostExeName();

// Human-readable name for an IID, for logging QueryInterface traffic.
// Returns a static string, never null. Unknown IIDs come back as their GUID.
// Defined in d3d12_proxy.cpp; only that DLL uses it.
const char* ProxyIidName(const IID& iid);
