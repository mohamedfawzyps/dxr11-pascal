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
//   2. dxr-tier-11.ini beside the shim
//   3. the built-in default
//
// Read once, lazily, and as late as possible: touching a file under the loader
// lock is how a proxy DLL deadlocks an application at startup.
//
// ONE setting breaks that rule on purpose, and it is worth knowing which. The
// log destination has to be resolved before the first line is written, and the
// first line is the start marker from DllMain. So `log` is read there. It is a
// read of one small text file, the same class of call as the fopen the log
// itself already makes from the same place, and nothing else is resolved that
// early.
#pragma once

#include <windows.h>

#include <string>

namespace cfg {

// A resolved switch, and where the value came from, so the log can say.
// `source` is one of "environment", "dxr-tier-11.ini" or "default".
struct Flag {
    bool value = false;
    const char* source = "default";
};

// `envName` is checked first, then `iniKey` in dxr-tier-11.ini, then `fallback`.
// Accepts 1/true/on/yes and 0/false/off/no, case insensitive.
Flag Get(const char* envName, const char* iniKey, bool fallback);

// The same resolution for a setting that is TEXT rather than a switch, such as
// where to put the log. Case is preserved, unlike the switches, because a path
// is not a keyword.
//
// Comes back as UTF-16, since a path can hold anything a user name can and
// narrow strings would mangle it. The file is read as UTF-8, falling back to
// the system code page for an .ini somebody saved from Notepad as ANSI.
struct Text {
    std::wstring value;
    const char* source = "default";
};

Text GetText(const char* envName, const char* iniKey);

// The directory the shim itself sits in, with a trailing backslash, or empty.
// This is where dxr-tier-11.ini is looked for, and it is what a RELATIVE path
// in one resolves against: a game's working directory is not something the
// person typing that path can predict.
std::wstring ShimDirectory();

}  // namespace cfg
