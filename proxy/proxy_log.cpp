// Shared logging, linked into BOTH proxy DLLs.
//
// d3d12.dll and dxgi.dll are separate modules, so each gets its own copy of
// this and its own handle on the log file. They append to the same file, which
// is what makes one log readable as one run: the timestamps order it, and the
// start and end markers bracket it.
//
// The log is appended to across runs on purpose. A crash leaves its evidence
// behind rather than being overwritten by the next launch, and that is only
// usable if every line says WHEN and the boundary between runs is visible.

#include "proxy_log.h"
#include "config.h"

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <string>

// Local time, not UTC. The person reading this is looking for what happened
// when their game stuttered, and they know that in local time.
static void ProxyStamp(char* out, size_t n) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::snprintf(out, n, "%04u-%02u-%02u %02u:%02u:%02u.%03u ",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds);
}

static std::wstring DefaultLogPath() {
    wchar_t p[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, p);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(p) + L"dxr-tier-11-proxy.log";
}

// Opens the log, and says on the returned file's first line if it did not go
// where it was asked to. A silent fallback would put the log somewhere the
// person is not looking, which is the one failure mode that matters here.
static FILE* OpenLogFile() {
    const cfg::Text want = cfg::GetText("DXR_TIER11_LOG", "log");
    std::string complaint;
    std::wstring path = want.value;

    if (!path.empty()) {
        // A RELATIVE path resolves against the shim's own directory, not the
        // process working directory, which for a game launched from Steam is
        // not something the person typing the path can predict.
        const bool absolute = (path.size() > 1 && path[1] == L':') ||
                              (path.size() > 1 && path[0] == L'\\' && path[1] == L'\\');
        if (!absolute) {
            const std::wstring dir = cfg::ShimDirectory();
            if (!dir.empty()) path = dir + path;
        }

        // A DIRECTORY means "put it in there", not "call the log that".
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
            path += L"dxr-tier-11-proxy.log";
        }
    }

    // _wfsopen with _SH_DENYNO, NOT _wfopen_s.
    //
    // fopen_s and _wfopen_s open EXCLUSIVELY, which is documented and is easy
    // to miss. It cost an hour here: d3d12.dll opened the log first, dxgi.dll's
    // open then failed, its fallback failed for the same reason, and the whole
    // DLL logged nothing at all while being demonstrably loaded. A component
    // that is silent looks like a component that did not run.
    FILE* fp = nullptr;
    if (!path.empty()) {
        fp = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
        if (!fp) complaint = "the log destination from the " +
                             std::string(want.source) +
                             " could not be opened, so this is the default one";
    }
    if (!fp) {
        const std::wstring fallback = DefaultLogPath();
        if (fallback.empty()) return nullptr;
        fp = _wfsopen(fallback.c_str(), L"a", _SH_DENYNO);
    }
    if (fp && !complaint.empty()) {
        const std::string line = "[dxr-tier-11-proxy-log] NOTE: " + complaint + "\n";
        std::fputs(line.c_str(), fp);
        OutputDebugStringA(line.c_str());
    }

    // Said through OutputDebugString only, because writing "the log is here"
    // into the log answers a question nobody who found the file is asking. A
    // debugger attached to a game that seems to produce no log is.
    if (want.source != std::string("default")) {
        OutputDebugStringA("[dxr-tier-11-proxy-log] log destination taken from the ");
        OutputDebugStringA(want.source);
        OutputDebugStringA("\n");
    }
    return fp;
}

void ProxyLog(const char* fmt, ...) {
    char stamp[32];
    ProxyStamp(stamp, sizeof(stamp));

    char buf[1024];
    const int pre = std::snprintf(buf, sizeof(buf), "%s", stamp);
    va_list a; va_start(a, fmt);
    std::vsnprintf(buf + pre, sizeof(buf) - pre, fmt, a);
    va_end(a);

    OutputDebugStringA(buf);
    static FILE* f = OpenLogFile();
    if (f) { std::fputs(buf, f); std::fflush(f); }
}

const char* ProxyHostExeName() {
    static char name[MAX_PATH] = {};
    if (!name[0]) {
        char path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        const char* base = path;
        for (DWORD i = 0; i < n; ++i)
            if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
        strcpy_s(name, MAX_PATH, base[0] ? base : "unknown.exe");
    }
    return name;
}
