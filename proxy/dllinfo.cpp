#include "dllinfo.h"

#include <cstdio>
#include <vector>

namespace dllinfo {

std::string OfFile(const std::wstring& path) {
    if (path.empty()) return "unknown";
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) return "unknown";
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return "unknown";
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
        return "unknown";
    char out[64];
    std::snprintf(out, sizeof(out), "%u.%u.%u.%u",
                  HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                  HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return out;
}

std::string OfModule(HMODULE h) {
    if (!h) return "not loaded";

    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(h, path, MAX_PATH);
    if (!n || n == MAX_PATH) return "loaded, path unknown";

    const std::wstring wide(path, n);
    const std::string version = OfFile(wide);

    // The path is the more useful half, since the same name can come from
    // System32, from beside the exe, or from an Agility subdirectory.
    std::string narrow;
    narrow.reserve(wide.size());
    for (wchar_t c : wide) narrow += (c < 128) ? static_cast<char>(c) : '?';

    return version + " (" + narrow + ")";
}

std::string OfLoadedModule(const wchar_t* name) {
    // GetModuleHandleW, not LoadLibraryW: this is a question, not a request.
    //
    // Note it matches on the BASE NAME, so asking it for "d3d12.dll" inside a
    // proxy called d3d12.dll answers with the proxy. Callers that want the real
    // one pass its handle to OfModule instead.
    return OfModule(GetModuleHandleW(name));
}

}  // namespace dllinfo
