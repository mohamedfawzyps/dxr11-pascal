// When the GPU has finished with a buffer the shim owns.
//
// Why this exists: a lowered pipeline builds a new shader table whenever the
// scene's record layout changes, and in Escher's open world that is almost
// every frame, because the instance count wobbles. 0.40.1 counted 10137
// tables in 7 minutes. An old table may still be read by a dispatch in
// flight, and nothing knew when that dispatch landed, so every one was held
// until the pipeline died: about 3 GB of upload heap by the end.
//
// So each dispatch records a USE: this command list reads this buffer. A use
// is stamped with a fence when its list is submitted, and it ends when the
// list can never run again (Reset or destroyed) AND the fence has passed. A
// closed list can be executed again, which is why submission alone does not
// end it: the same rule 0.38.1 learned for readbacks, "recorded" is not
// "submitted", taken one step further.
//
// Holds NO reference. The buffer's owner keeps it alive and asks Busy()
// before reusing or releasing it.
#pragma once

#include <d3d12.h>

namespace gpuhold {

// `owner` recorded work that reads `res`. Cheap to repeat for the same pair.
void Use(const void* res, const void* owner);

// These lists went out on `queue`. Wrapped lists are recognised; anything
// else is ignored. Also retires uses whose fence has passed.
void AfterSubmit(ID3D12CommandQueue* queue, ID3D12CommandList* const* lists, UINT count);

// A shim-owned list that went out on `queue` once and will never run again.
void SubmittedOnce(ID3D12CommandQueue* queue, const void* owner);

// `owner` was Reset or destroyed, so it cannot run again. Uses it never
// submitted end now; submitted ones end when their fence passes.
void Detach(const void* owner);

// Could any GPU work, submitted or still to be, read `res`?
bool Busy(const void* res);

}  // namespace gpuhold
