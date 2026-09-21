// The shim's version, in one place.
//
// It is logged on attach for a practical reason: a bug report about this
// project arrives as a log file, and the first thing anyone needs to know is
// which build produced it. A proxy DLL gets copied next to an executable and
// then forgotten, so the binary cannot be trusted to be the one the reporter
// thinks it is.
//
// Keep it in step with the tag and with CHANGELOG.md.
#pragma once

#define DXR11_VERSION "0.9.0"
