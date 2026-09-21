// Phase 4 (S1) - queue vtable hook. See queue_hook.h for why.

#include "queue_hook.h"
#include "proxy_log.h"

#include <windows.h>
#include <vector>

namespace {

// ID3D12CommandQueue vtable, counted through IUnknown (QueryInterface, AddRef,
// Release), ID3D12Object (GetPrivateData, SetPrivateData,
// SetPrivateDataInterface, SetName), ID3D12DeviceChild (GetDevice) and
// ID3D12Pageable (nothing), then the queue's own methods: UpdateTileMappings,
// CopyTileMappings, ExecuteCommandLists.
//
// As with the command list hook, this number is not trusted. The self-test
// below proves it before the patch is allowed to stay.
const size_t kExecuteCommandListsSlot = 10;

typedef void (STDMETHODCALLTYPE *PFN_ExecuteCommandLists)(
    ID3D12CommandQueue* self, UINT NumCommandLists,
    ID3D12CommandList* const* ppCommandLists);

PFN_ExecuteCommandLists g_original = nullptr;
void**                  g_hookedVTable = nullptr;
bool                    g_active = false;
SRWLOCK                 g_lock = SRWLOCK_INIT;

thread_local bool g_selfTestRunning = false;
thread_local bool g_selfTestFired   = false;

void STDMETHODCALLTYPE Hook_ExecuteCommandLists(
        ID3D12CommandQueue* self, UINT NumCommandLists,
        ID3D12CommandList* const* ppCommandLists) {
    if (g_selfTestRunning) { g_selfTestFired = true; return; }

    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        ProxyLog("[dxr11-proxy] ExecuteCommandLists intercepted (first real call), n=%u\n",
                 NumCommandLists);

    // Once command lists are wrapped, this is where they are swapped back for
    // the real ones. Until then there is nothing to unwrap, so forward as-is
    // and keep the allocation out of the hot path.
    g_original(self, NumCommandLists, ppCommandLists);
}

bool PatchSlot(void** vtable, size_t index, void* replacement, void** outOriginal) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect))
        return false;
    if (outOriginal) *outOriginal = vtable[index];
    vtable[index] = replacement;
    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
    return true;
}

} // namespace

bool Dxr11InstallQueueHook(ID3D12CommandQueue* sample) {
    if (!sample) return false;
    void** vt = *reinterpret_cast<void***>(sample);

    AcquireSRWLockExclusive(&g_lock);
    if (g_hookedVTable == vt) { ReleaseSRWLockExclusive(&g_lock); return g_active; }
    if (g_hookedVTable) {
        // Measured to be one shared vtable for every queue, so a second distinct
        // one means the assumption has broken on this runtime. Say so.
        ProxyLog("[dxr11-proxy] queue hook: a SECOND queue vtable appeared (%p, had %p). "
                 "The shared-vtable assumption does not hold here.\n",
                 (void*)vt, (void*)g_hookedVTable);
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    void* original = nullptr;
    if (!PatchSlot(vt, kExecuteCommandListsSlot, (void*)&Hook_ExecuteCommandLists, &original)) {
        ProxyLog("[dxr11-proxy] queue hook: VirtualProtect failed on vtable %p\n", (void*)vt);
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_original = (PFN_ExecuteCommandLists)original;
    g_hookedVTable = vt;

    // Self-test: submitting zero command lists is a legal no-op, and the hook
    // returns before forwarding while the probe flag is set, so nothing reaches
    // the runtime either way.
    g_selfTestFired = false;
    g_selfTestRunning = true;
    sample->ExecuteCommandLists(0, nullptr);
    g_selfTestRunning = false;

    if (!g_selfTestFired) {
        PatchSlot(vt, kExecuteCommandListsSlot, original, nullptr);
        g_original = nullptr;
        g_hookedVTable = nullptr;
        ProxyLog("[dxr11-proxy] queue hook: SELF-TEST FAILED, slot %zu is not "
                 "ExecuteCommandLists. Hook removed.\n", kExecuteCommandListsSlot);
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }

    g_active = true;
    ProxyLog("[dxr11-proxy] queue hook installed: vtable %p slot %zu, self-test passed\n",
             (void*)vt, kExecuteCommandListsSlot);
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool Dxr11QueueHookActive() { return g_active; }
