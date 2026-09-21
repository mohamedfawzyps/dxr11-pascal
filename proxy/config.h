// Where the switches come from, and in what order.
//
// Somebody who downloaded a release and copied a DLL into a game folder has
// already opted in. Making them set an environment variable as well mostly
// produces "it does nothing" reports, and a launcher or a terminal is not
// something every user has. So a plain text file beside the DLL is the third
// way to set anything, and it is the one a person without a terminal can use.
//
// Resolution order, first match wins:
//
//   1. the environment, which is explicit and beats everything. The test
//      scripts rely on this, so a stray .ini can never change what they measure
//   2. dxr11.ini beside the shim
//   3. the built-in default
//
// Read once, lazily. Never from DllMain: touching a file under the loader lock
// is how a proxy DLL deadlocks an application at startup.
#pragma once

#include <windows.h>

namespace cfg {

// A resolved switch, and where the value came from, so the log can say.
// `source` is one of "environment", "dxr11.ini" or "default".
struct Flag {
    bool value = false;
    const char* source = "default";
};

// `envName` is checked first, then `iniKey` in dxr11.ini, then `fallback`.
// Accepts 1/true/on/yes and 0/false/off/no, case insensitive.
Flag Get(const char* envName, const char* iniKey, bool fallback);

}  // namespace cfg
