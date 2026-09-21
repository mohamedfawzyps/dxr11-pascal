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
    std::ifstream f(dir + L"dxr11.ini");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        g_ini[Lower(Trim(line.substr(0, eq)))] = Lower(Trim(line.substr(eq + 1)));
    }
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
    if (it != g_ini.end() && Truthy(it->second, &parsed))
        return Flag{ parsed, "dxr11.ini" };

    return Flag{ fallback, "default" };
}

}  // namespace cfg
