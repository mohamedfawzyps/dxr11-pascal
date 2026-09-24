"""NVAPI shader extension calls that ask a RayQuery for a cluster ID.

NVIDIA's HLSL extensions are not opcodes. Each call is written as ordinary
stores to one UAV, `RWStructuredBuffer<NvShaderExtnStruct>`, and the driver
recognises the pattern and replaces it, but ONLY while the application has
registered that UAV's slot with NvAPI_D3D12_SetNvShaderExtnSlotSpace*. Unreal
registers it around every compute pipeline it creates with a vendor extension,
so the shim's CreateStateObject, made inside that call, has it registered too.

A RayQuery cluster-ID call moved into a hit shader, or left in a raygen after
its query is gone, names a query that no longer exists, and the driver's
compiler dies on it: DXGI_ERROR_DRIVER_INTERNAL_ERROR, 3 of 3 Escher runs at
0.41.0, 10 of 10 offline with the slot registered, 0 of 24 without.

One call is, from nvHLSLExtnsInternal.h:

    uint index = g_NvidiaExt.IncrementCounter();
    g_NvidiaExt[index].opcode = OP;
    g_NvidiaExt[index].src0u.x = rq.RayFlags();
    return g_NvidiaExt.IncrementCounter();     // the value

For OP 94 (candidate) and 95 (committed) the value is a cluster ID, and
without cluster operations every geometry answers 0xFFFFFFFF, "not a cluster".
Measured on the GTX 1070: NvAPI_D3D12_GetRaytracingCaps(CLUSTER_OPERATIONS)
is 0x0, CAP_NONE, and Unreal uses 0xFFFFFFFF itself when cluster ops are off.
So each such call becomes that constant and its stores go. Any other use of
the extension UAV is refused: what it means depends on the driver, and moving
it into a DXR 1.0 shader is not something this shim can show is the same.

Runs on NORMALISED text, before analysis, so the rest of the rewriter never
sees the calls. A module with no NvShaderExtnStruct UAV comes back unchanged.
"""

import re

from rayquery import Unsupported

EXT_TYPE = '%"class.RWStructuredBuffer<NvShaderExtnStruct>"'
CLUSTER_ID_OPS = (94, 95)
NO_CLUSTER = '-1'   # 0xFFFFFFFF as LLVM prints an i32

_RECORD = re.compile(r'^!\d+ = !\{i32 (\d+), ' + re.escape(EXT_TYPE) +
                     r'\* [^,]+, !"[^"]*", i32 (\d+), i32 (\d+), ')
_DEF = re.compile(r'^\s*(%[\w.]+) = ')
_COUNTER = re.compile(r'@dx\.op\.bufferUpdateCounter\(i32 70, %dx\.types\.Handle (%[\w.]+), i8 1\)')
_STORE = re.compile(r'@dx\.op\.(?:rawBufferStore|bufferStore)\.\w+\(i32 (?:140|69), '
                    r'%dx\.types\.Handle (%[\w.]+), i32 (%[\w.]+), i32 (\d+), i32 (%?[\w.-]+),')
_ANNOTATE = re.compile(r'@dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle (%[\w.]+),')
_HANDLE_USE = re.compile(r'%dx\.types\.Handle (%[\w.]+)')
_CALLEE = re.compile(r' @(dx\.op\.[\w.]+)\(')


def _ends_block(line):
    s = line.strip()
    return (s.startswith(('br ', 'ret', 'switch ', 'unreachable')) or
            s.endswith(':') or s.startswith('; <label>:') or s == '}')


def _uses(name):
    return re.compile(re.escape(name) + r'(?![\w.])')


def fold(text):
    lines = text.split('\n')
    bindings = set()
    for ln in lines:
        m = _RECORD.match(ln)
        if m:
            bindings.add((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    if not bindings:
        return text

    # The handles that reach the extension UAV: made from its binding, then
    # annotated any number of times.
    ext = set()
    for ln in lines:
        d = _DEF.match(ln)
        if not d:
            continue
        for rid, space, lb in bindings:
            if ('@dx.op.createHandleFromBinding(i32 217, %%dx.types.ResBind { i32 %d, i32 '
                % lb in ln and ', i32 %d, i8 1 }' % space in ln) or \
               '@dx.op.createHandle(i32 57, i8 1, i32 %d, ' % rid in ln:
                ext.add(d.group(1))
    changed = True
    while changed:
        changed = False
        for ln in lines:
            d, a = _DEF.match(ln), _ANNOTATE.search(ln)
            if d and a and a.group(1) in ext and d.group(1) not in ext:
                ext.add(d.group(1))
                changed = True

    drop, values, indices = set(), {}, []
    i = 0
    while i < len(lines):
        c = _COUNTER.search(lines[i])
        d = _DEF.match(lines[i])
        if not (c and d and c.group(1) in ext) or i in drop:
            i += 1
            continue
        index, stores, second, op = d.group(1), [], None, None
        j = i + 1
        while j < len(lines) and not _ends_block(lines[j]):
            s = _STORE.search(lines[j])
            if s and s.group(1) in ext and s.group(2) == index:
                stores.append(j)
                if s.group(3) == '0':
                    op = s.group(4)
            c2, d2 = _COUNTER.search(lines[j]), _DEF.match(lines[j])
            if c2 and d2 and c2.group(1) in ext:
                second = j
                break
            j += 1
        if op is None or not op.lstrip('-').isdigit():
            raise Unsupported('uses the NVAPI shader extension UAV in a way that '
                              'is not a recognisable call; the driver reads it '
                              'as an intrinsic, and it cannot be moved')
        if int(op) not in CLUSTER_ID_OPS or second is None:
            raise Unsupported('uses NVAPI shader extension op %s, which has no '
                              'DXR 1.0 lowering here (only the RayQuery cluster '
                              'ID calls, 94 and 95, are folded)' % op)
        drop.update([i, second] + stores)
        values[_DEF.match(lines[second]).group(1)] = NO_CLUSTER
        indices.append(index)
        i = second + 1

    out = [ln for k, ln in enumerate(lines) if k not in drop]
    for name, const in values.items():
        pat = _uses(name)
        out = [pat.sub(const, ln) for ln in out]
    for idx in indices:
        pat = _uses(idx)
        if any(pat.search(ln) for ln in out):
            raise Unsupported('an NVAPI call index is used beyond the call')

    # Annotations of the extension handle that nothing reads now go too, and
    # anything still reading one is a use this pass did not account for.
    changed = True
    while changed:
        changed = False
        for k, ln in enumerate(out):
            d, a = _DEF.match(ln), _ANNOTATE.search(ln)
            if d and a and d.group(1) in ext:
                pat = _uses(d.group(1))
                if not any(pat.search(o) for n, o in enumerate(out) if n != k):
                    del out[k]
                    changed = True
                    break
    for ln in out:
        d = _DEF.match(ln)
        if d and d.group(1) in ext:
            continue
        for h in _HANDLE_USE.findall(ln):
            if h in ext:
                raise Unsupported('uses the NVAPI shader extension UAV outside '
                                  'a cluster ID call, which cannot be moved')

    # A declaration nothing calls any more is itself a validation error. Only
    # the functions this pass removed calls to are considered, with the
    # attribute comment DXC writes above each.
    for fn in sorted({_CALLEE.search(lines[k]).group(1) for k in drop}):
        called = '@%s(' % fn
        if any(called in ln and not ln.startswith('declare ') for ln in out):
            continue
        for k, ln in enumerate(out):
            if ln.startswith('declare ') and called in ln:
                first = k - 1 if k and out[k - 1].startswith('; Function Attrs:') else k
                del out[first:k + 1]
                break
    return '\n'.join(out)
