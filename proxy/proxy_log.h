// Shared logging for the proxy. Writes to %TEMP%\dxr11_proxy.log and
// OutputDebugString. Definitions live in d3d12_proxy.cpp.
#pragma once

#include <guiddef.h>

void ProxyLog(const char* fmt, ...);

// Human-readable name for an IID, for logging QueryInterface traffic.
// Returns a static string, never null. Unknown IIDs come back as their GUID.
const char* ProxyIidName(const IID& iid);
