// Phase 4 (S1) - vtable hook on ID3D12CommandQueue::ExecuteCommandLists.
//
// Half of the hybrid design. The other half is the command list wrapper.
//
// Why this split, and why each half is the way it is, was measured rather than
// chosen. See docs/phase4-indirect-design.md.
//
//   The QUEUE cannot be wrapped. DXGI consumes a command queue: the app passes
//   it to CreateSwapChainForHwnd, DXGI reaches the real object through
//   QueryInterface, and with a wrapper in the way the first Present
//   access-violates. Both Microsoft samples died with
//   STATUS_FATAL_USER_CALLBACK_EXCEPTION the moment the window was focused.
//   But the queue vtable IS hookable: one shared image-address vtable is used
//   by every queue of every type, DIRECT, COMPUTE and COPY alike, and it does
//   not change across ExecuteCommandLists or Signal. Verified with
//   `tier11probe.exe -queuevtable`.
//
//   The LIST cannot usefully be hooked. A command list swaps to a per-object,
//   heap-allocated vtable, so the shared image vtable it has when
//   CreateCommandList returns is abandoned before the app records anything. A
//   hook installed there self-tests cleanly and then never sees a real call.
//   Verified with `tier11probe.exe -hooktest`. But wrapping a list is safe,
//   because nothing outside D3D12 ever holds one.
//
// So: wrap the list, hook the queue. The hook exists to unwrap our command
// lists on their way into the real ExecuteCommandLists.

#pragma once

#include <d3d12.h>

// Installs the ExecuteCommandLists hook, using `sample` to locate the vtable.
// Idempotent. Returns false if the self-test failed, in which case nothing was
// left patched and command list wrapping must stay off.
bool Dxr11InstallQueueHook(ID3D12CommandQueue* sample);

// True once the hook is installed and self-tested.
bool Dxr11QueueHookActive();
