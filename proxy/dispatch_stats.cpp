#include "dispatch_stats.h"

#include "proxy_log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace dstats {
namespace {

std::atomic<uint64_t> g_count[kCount];
std::atomic<ULONGLONG> g_nextTick{0};
const ULONGLONG kEveryMs = 10000;

std::mutex g_printLock;
uint64_t g_printed[kCount] = {};

void Line(const uint64_t* c, const char* when, const std::string& live) {
    const uint64_t refused = c[kRefusedCrossLive] + c[kRefusedOneTlas] + c[kRefusedProcedural];
    ProxyLog("[dxr-tier-11-proxy-log] stats%s: lowered dispatches DRAWN %llu (%llu indirect), "
             "REFUSED %llu (two live structures disagree %llu, one structure disagrees %llu, "
             "procedural collapse %llu), table unbuildable %llu, held back %llu, indirect "
             "with zero groups %llu; tables built %llu, reused %llu; top-level reads: new "
             "%llu, changed %llu, unchanged %llu%s%s\n",
             when,
             (unsigned long long)c[kDrawn], (unsigned long long)c[kIndirect],
             (unsigned long long)refused, (unsigned long long)c[kRefusedCrossLive],
             (unsigned long long)c[kRefusedOneTlas], (unsigned long long)c[kRefusedProcedural],
             (unsigned long long)c[kSkippedTable], (unsigned long long)c[kHeldBack],
             (unsigned long long)c[kIndirectEmpty],
             (unsigned long long)c[kTableNew], (unsigned long long)c[kTableCached],
             (unsigned long long)c[kTlasNew], (unsigned long long)c[kTlasChanged],
             (unsigned long long)c[kTlasSame],
             live.empty() ? "" : "; live: ", live.c_str());
}

}  // namespace

void Add(Counter c) { g_count[c].fetch_add(1, std::memory_order_relaxed); }

void Tick(std::string (*live)()) {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG due = g_nextTick.load(std::memory_order_relaxed);
    if (now < due) return;
    if (!g_nextTick.compare_exchange_strong(due, now + kEveryMs)) return;

    uint64_t c[kCount];
    for (int i = 0; i < kCount; ++i) c[i] = g_count[i].load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(g_printLock);
    bool moved = false;
    for (int i = 0; i < kCount; ++i) moved |= c[i] != g_printed[i];
    if (!moved) return;
    for (int i = 0; i < kCount; ++i) g_printed[i] = c[i];
    Line(c, "", live ? live() : std::string());
}

void Final() {
    uint64_t c[kCount];
    for (int i = 0; i < kCount; ++i) c[i] = g_count[i].load(std::memory_order_relaxed);
    Line(c, " at exit", std::string());
}

}  // namespace dstats
