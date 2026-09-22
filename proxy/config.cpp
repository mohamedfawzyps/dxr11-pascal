#include "config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace cfg {
namespace {

std::once_flag g_once;
std::map<std::string, std::string> g_ini;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// The shim's own directory, found from an address inside this module rather
// than from a handle passed in. A proxy DLL is loaded under a name it does not
// choose, so asking by name is not an option.
std::wstring OwnDirectory() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OwnDirectory), &self))
        return L"";
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : p.substr(0, slash + 1);
}

void LoadIni() {
    const std::wstring dir = OwnDirectory();
    if (dir.empty()) return;
    std::ifstream f(dir + L"dxr-tier-11.ini");
    if (!f) return;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        // A UTF-8 BOM would otherwise end up glued to the first key, and the
        // setting would be silently ignored rather than reported as wrong.
        if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
            line.erase(0, 3);
        first = false;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        // The KEY is lowered, the VALUE is not: a switch is a keyword and a
        // path is not. Truthy() lowers what it is given.
        g_ini[Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
}

std::wstring Decode(const std::string& s, UINT codepage, DWORD flags) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(codepage, flags, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(codepage, flags, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

std::wstring Widen(const char* ascii) { return Decode(ascii, CP_ACP, 0); }

// An .ini value to a path. Written as UTF-8 by the setup tool, but somebody
// editing it by hand in Notepad may well have saved it as ANSI, so a strict
// UTF-8 decode that fails falls back rather than producing nothing. Getting
// this wrong shows up as a log that silently goes to the wrong place.
std::wstring FromIniBytes(const std::string& s) {
    std::wstring w = Decode(s, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (w.empty() && !s.empty()) w = Decode(s, CP_ACP, 0);
    return w;
}

bool Truthy(const std::string& v, bool* out) {
    if (v == "1" || v == "true" || v == "on" || v == "yes") { *out = true; return true; }
    if (v == "0" || v == "false" || v == "off" || v == "no") { *out = false; return true; }
    return false;   // present but unreadable: fall through rather than guess
}

}  // namespace

Flag Get(const char* envName, const char* iniKey, bool fallback) {
    std::call_once(g_once, LoadIni);

    char buf[32] = {};
    const DWORD n = GetEnvironmentVariableA(envName, buf, sizeof(buf));
    bool parsed = false;
    if (n > 0 && n < sizeof(buf) && Truthy(Lower(buf), &parsed))
        return Flag{ parsed, "environment" };

    auto it = g_ini.find(Lower(iniKey));
    if (it != g_ini.end() && Truthy(Lower(it->second), &parsed))
        return Flag{ parsed, "dxr-tier-11.ini" };

    return Flag{ fallback, "default" };
}

Text GetText(const char* envName, const char* iniKey) {
    std::call_once(g_once, LoadIni);

    // A path can be long and can hold anything a user name can, so this is
    // wide from the environment onwards. GetEnvironmentVariableW returns the
    // required size when the buffer is too small, which is how the length is
    // discovered rather than guessed.
    const std::wstring wideEnv = Widen(envName);
    DWORD n = GetEnvironmentVariableW(wideEnv.c_str(), nullptr, 0);
    if (n > 1) {
        std::wstring buf(n, L'\0');
        n = GetEnvironmentVariableW(wideEnv.c_str(), &buf[0], n);
        buf.resize(n);
        if (!buf.empty()) return Text{ buf, "environment" };
    }

    auto it = g_ini.find(Lower(iniKey));
    if (it != g_ini.end() && !it->second.empty())
        return Text{ FromIniBytes(it->second), "dxr-tier-11.ini" };

    return Text{ std::wstring(), "default" };
}

std::wstring ShimDirectory() { return OwnDirectory(); }

}  // namespace cfg
