#include "shader_dump.h"

#include "config.h"
#include "proxy_log.h"

#include <share.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace shdump {
namespace {

std::mutex g_lock;
bool g_ready = false;
std::wstring g_dir;          // empty means off
unsigned g_written = 0;

// Enough to diagnose a refusal pattern, few enough that nobody's disk fills up
// while they are playing. Unreal refused 158 shaders in a session and the
// distinct SHAPES numbered about five.
const unsigned kMax = 64;

void InitLocked() {
    g_ready = true;
    const cfg::Text want = cfg::GetText("DXR_TIER11_DUMP", "dump");
    if (want.value.empty()) return;

    std::wstring dir = want.value;
    // Relative resolves against the shim's own directory, same rule as the log:
    // a game's working directory is not something the person typing the path
    // can predict.
    const bool absolute = (dir.size() > 1 && dir[1] == L':') ||
                          (dir.size() > 1 && dir[0] == L'\\' && dir[1] == L'\\');
    if (!absolute) {
        const std::wstring base = cfg::ShimDirectory();
        if (!base.empty()) dir = base + dir;
    }
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';

    // One level only. A path whose parent does not exist is a typo, and
    // creating a tree for it would hide that.
    CreateDirectoryW(dir.c_str(), nullptr);
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ProxyLog("[dxr-tier-11-proxy-log] shader dumping is ON (from %s) but the "
                 "directory could not be created; nothing will be written\n",
                 want.source);
        return;
    }
    g_dir = dir;
    ProxyLog("[dxr-tier-11-proxy-log] shader dumping is ON (from %s). Up to %u "
             "refused shaders will be written as .dxil containers, each with a "
             ".txt saying why. Run them back through `dxrw rewrite` to reproduce "
             "the refusal offline.\n", want.source, kMax);
}

}  // namespace

void Refused(const void* container, size_t size, const char* why) {
    if (!container || size == 0) return;

    std::lock_guard<std::mutex> g(g_lock);
    if (!g_ready) InitLocked();
    if (g_dir.empty()) return;
    if (g_written >= kMax) return;

    const unsigned n = g_written++;
    wchar_t stem[64];
    swprintf_s(stem, L"refused_%03u", n);

    FILE* f = _wfsopen((g_dir + stem + L".dxil").c_str(), L"wb", _SH_DENYNO);
    if (f) { std::fwrite(container, 1, size, f); std::fclose(f); }

    f = _wfsopen((g_dir + stem + L".txt").c_str(), L"w", _SH_DENYNO);
    if (f) { std::fputs(why ? why : "(no reason given)", f); std::fclose(f); }

    if (n + 1 == kMax)
        ProxyLog("[dxr-tier-11-proxy-log] shader dump limit of %u reached; no more "
                 "will be written this run\n", kMax);
}

}  // namespace shdump
