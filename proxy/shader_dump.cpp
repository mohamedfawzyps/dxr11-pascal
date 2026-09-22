#include "shader_dump.h"

#include "config.h"
#include "proxy_log.h"

#include <share.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace shdump {
namespace {

std::mutex g_lock;
bool g_ready = false;
std::wstring g_dir;          // empty means off
unsigned g_written = 0;
unsigned g_lowered = 0;      // its own counter, see shader_dump.h

// Bounded on purpose. An engine makes a few dozen root signatures; anything
// that makes thousands is not something to keep blobs for.
const size_t kMaxRootSigs = 512;
std::map<void*, std::vector<unsigned char>> g_rootSigs;
int g_wantRootSigs = -1;     // -1 not asked yet

bool WriteFile(const std::wstring& path, const void* p, size_t n) {
    FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
    if (!f) return false;
    std::fwrite(p, 1, n, f);
    std::fclose(f);
    return true;
}

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

    WriteFile(g_dir + stem + L".dxil", container, size);

    FILE* f = _wfsopen((g_dir + stem + L".txt").c_str(), L"w", _SH_DENYNO);
    if (f) { std::fputs(why ? why : "(no reason given)", f); std::fclose(f); }

    if (n + 1 == kMax)
        ProxyLog("[dxr-tier-11-proxy-log] shader dump limit of %u reached; no more "
                 "will be written this run\n", kMax);
}

void NoteRootSignature(void* rs, const void* blob, size_t size) {
    if (!rs || !blob || size == 0) return;
    std::lock_guard<std::mutex> g(g_lock);
    if (g_wantRootSigs < 0)
        g_wantRootSigs = cfg::GetText("DXR_TIER11_DUMP", "dump").value.empty() ? 0 : 1;
    if (!g_wantRootSigs) return;
    if (g_rootSigs.size() >= kMaxRootSigs && !g_rootSigs.count(rs)) return;
    const unsigned char* p = static_cast<const unsigned char*>(blob);
    g_rootSigs[rs].assign(p, p + size);
}

void Lowered(const void* original, size_t originalSize,
             const void* lowered, size_t loweredSize,
             void* rootSignature, const char* shape) {
    if (!original || !lowered || originalSize == 0 || loweredSize == 0) return;

    std::lock_guard<std::mutex> g(g_lock);
    if (!g_ready) InitLocked();
    if (g_dir.empty()) return;
    if (g_lowered >= kMax) return;

    const unsigned n = g_lowered++;
    wchar_t stem[64];
    swprintf_s(stem, L"lowered_%03u", n);

    // Both halves. The input is what `dxrw rewrite` needs to reproduce the
    // lowering; the output is what CreateStateObject was actually handed, and
    // it is the one the driver saw.
    WriteFile(g_dir + stem + L".in.dxil", original, originalSize);
    WriteFile(g_dir + stem + L".out.dxil", lowered, loweredSize);

    // The global root signature the state object was built with. Without it
    // the library cannot be replayed at all: CreateStateObject needs one and
    // the bindings have to match what the DXIL declares.
    auto it = g_rootSigs.find(rootSignature);
    if (it != g_rootSigs.end())
        WriteFile(g_dir + stem + L".rs.bin", it->second.data(), it->second.size());

    if (shape && *shape)
        WriteFile(g_dir + stem + L".shape.txt", shape, std::strlen(shape));

    if (n + 1 == kMax)
        ProxyLog("[dxr-tier-11-proxy-log] lowered dump limit of %u reached; no more "
                 "will be written this run\n", kMax);
}

}  // namespace shdump
