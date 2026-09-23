// The shim's version, in one place.
//
// It is logged on attach for a practical reason: a bug report about this
// project arrives as a log file, and the first thing anyone needs to know is
// which build produced it. A proxy DLL gets copied next to an executable and
// then forgotten, so the binary cannot be trusted to be the one the reporter
// thinks it is.
//
// It also goes into the DLL's own version RESOURCE, so the file answers the
// question without being run. The setup tool reads the versions of every DLL
// in the game folder, and a shim that is the only one with no version at all
// is the one most worth identifying.
//
// Keep all three in step with the tag and with CHANGELOG.md.
#pragma once

#define DXR_TIER11_VERSION       "0.40.0"
#define DXR_TIER11_VERSION_COMMA 0, 40, 0, 0
