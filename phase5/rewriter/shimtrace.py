"""Point an application's TraceRay calls at the shim's copy of the scene.

For GeometryIndex() in an application's own hit shaders (see geomidx.py),
when one hit group record is reached by several geometries, a TraceRay
multiplier of 0 for instance, no record can say which geometry was hit. The
shim then gives the dispatch a layout of its own, in which every (instance,
geometry, TraceRay argument pair) has its own record:

    record = r + K * geometry + instance base

That needs the hardware to compute it, so the shim builds a copy of the
application's top-level structure whose instances carry the shim's bases,
and a VARIANT of the pipeline whose TraceRay calls trace that copy with
RayContributionToHitGroupIndex r (the index of the call's original pair in
the pipeline's list) and a multiplier of K (the number of pairs).

This pass makes the variant's library: every TraceRay traces a structure
bound at t0, space GI_SPACE (a LOCAL root descriptor the shim appends to
the raygen's local root signature) with (r, K). Everything else, the ray,
the flags, the mask, the miss index and the payload, is untouched. Shapes
copied from DXC, phase5/cases/reference/lib_shimtlas_ref.hlsl:

    lib_6_5  @T = external constant %struct.RaytracingAccelerationStructure;
             load, createHandleForLib.struct.RaytracingAccelerationStructure
    lib_6_6  @T = external constant %dx.types.Handle; load,
             createHandleForLib.dx.types.Handle, annotateHandle {16, 0}

and an SRV record {id, global, name, space, 0, 1, kind 16, 0, !{i32 0, i32 4}}.

    python phase5/rewriter/shimtrace.py <in.ll> <out.ll> R,M [R,M ...]
    python phase5/rewriter/shimtrace.py <in.ll> <out.ll> --copies N

With arguments computed at run time (or more than 15 pairs) each call reads
its (r, K) and scene copy from a table of the shim's, a ByteAddressBuffer at
tN, space GI_SPACE, after the N copies at t0..tN-1 (0.57.0). Shapes from
DXC: rawBufferLoad (139) on createHandleForLib.struct.ByteAddressBuffer at
6.5, annotateHandle {11, 0} at 6.6, an SRV record of kind 11 with no extra;
the copy is chosen by a switch.

Several scenes (0.59.0): with `slots` S, call n traces the copy of scene slot
slot_of[n]; slot j's structure c is at t(j * C + c), C = max(copies, 1), the
table at t(S * C). Slot 0 keeps the names @dxr11.tlas[.c], slot j is
@dxr11.tlas.sJ[.c]. With one slot the output is what it was.

    python phase5/rewriter/shimtrace.py <in.ll> <out.ll> ... --slots S j,j,...

A scene picked per ray (0.60.0): slot j can hold caps[j] scenes, its SUB-SLOTS,
when its calls take the scene from an array at a dynamic element or from the
descriptor heap. The call's KEY (the element, or the heap index, read off the
handle's definition) is looked up in the shim's key table, a ByteAddressBuffer
@dxr11.keys after the pair table, which the shim writes per dispatch: dword j
the start of slot j's list (in dwords), there n, then n (key, sub-slot) pairs
sorted by key. A binary search finds the sub-slot, and a switch traces its
structure. Sub-slot p of slot j is @dxr11.tlas[.sJ].kP[.c] (p 0 keeps the
slot's name), structure c of it at t((subAt[j] + p) * C + c). With every
capacity 1 the output is what it was.

    python phase5/rewriter/shimtrace.py <in.ll> <out.ll> ... --caps c,c,...
"""

import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lower import _resolve_attrs, _is_sm66, HANDLE_TYPE
from geomidx import GI_SPACE, Unsupported

TLAS_GLOBAL = '@dxr11.tlas'
TLAS_NAME = 'dxr11.tlas'
LUT_GLOBAL = '@dxr11.lut'
KEYS_GLOBAL = '@dxr11.keys'
RAS = '%struct.RaytracingAccelerationStructure'
BAB = '%struct.ByteAddressBuffer'
RESRET = '%dx.types.ResRet.i32'
RESPROPS = '%dx.types.ResourceProperties'

_LABEL = re.compile(r'^([A-Za-z$._][\w$.]*):')
_TRACE = re.compile(r'^(\s*)call void @dx\.op\.traceRay\.([^(]+)\(i32 157, '
                    r'%dx\.types\.Handle (%[\w.]+), i32 ([^,]+), i32 ([^,]+), '
                    r'i32 ([^,]+), i32 ([^,]+), (.*)$')
_DEF = re.compile(r'^\s+(%[\w.]+) = (.*)$')
_OPND = re.compile(r'\(i32 \d+, %[^ ]+ (%[\w.]+)')
_HEAPIDX = re.compile(r'@dx\.op\.createHandleFromHeap\(i32 218, i32 ([^,]+),')
_LOADPTR = re.compile(r'\* (%[\w.]+), align')
_NONUNIFORM = re.compile(r', !dx\.nonuniform !\d+$')
_LASTIDX = re.compile(r', i32 ([^,]+)$')


def _key_of(defs, h):
    """The KEY a TraceRay's scene handle is picked by: the index of
    createHandleFromHeap, or the element of the array its resource was loaded
    from. None when it is neither."""
    for _ in range(8):
        d = defs.get(h)
        if d is None:
            return None
        m = _HEAPIDX.search(d)
        if m:
            return m.group(1)
        if '@dx.op.annotateHandle(' in d or '@dx.op.createHandleForLib' in d:
            m = _OPND.search(d)
            if not m:
                return None
            h = m.group(1)
            continue
        if d.startswith('load '):
            m = _LOADPTR.search(d)
            g = defs.get(m.group(1)) if m else None
            if g is None or not g.startswith('getelementptr '):
                return None
            m = _LASTIDX.search(_NONUNIFORM.sub('', g))
            return m.group(1) if m else None
        return None
    return None


def retrace(text, pairs, copies=0, slot_of=None, slots=1, caps=None):
    """Returns (text, calls). With `copies` 0, `pairs` is the pipeline's list
    of (R, M), low 4 bits each, and a call's r is the index of its pair. With
    `copies` N >= 1 (arguments computed at run time, or more than 15 pairs)
    each call reads its pair from the shim's table at tN, indexed by
    16 * (R & 15) + (M & 15): r in bits 0-7, the multiplier in 8-15 and the
    scene copy, one of t0..tN-1, from bit 16. No TraceRay: unchanged."""
    lines = text.split('\n')
    if not any(_TRACE.match(l) for l in lines):
        return text, 0
    if (TLAS_GLOBAL + ' ' in text or TLAS_GLOBAL + '.' in text
            or LUT_GLOBAL + ' ' in text or KEYS_GLOBAL + ' ' in text):
        raise Unsupported('the library already defines %s' % TLAS_GLOBAL)
    sm66 = _is_sm66(text)
    k = len(pairs)
    slots = slots or 1
    caps = list(caps) if caps else [1] * slots
    if len(caps) != slots or min(caps) < 1:
        raise Unsupported('the scene slots\' capacities do not match the slots')
    sub_at = [sum(caps[:j]) for j in range(slots)]
    subs = sum(caps)
    keyed = subs > slots
    cc = copies or 1
    slot = [0]   # the call being rewritten's
    # Every function's definitions, for the key of a keyed call.
    defs, fn_defs = [], None
    for line in lines:
        if line.startswith('define '):
            fn_defs = {}
            defs.append(fn_defs)
        elif fn_defs is not None:
            dm = _DEF.match(line)
            if dm:
                fn_defs[dm.group(1)] = dm.group(2)
    fn = [-1]

    def bab_handle(o, t, name, glob, props_comment):
        if sm66:
            o += [
                '  %s.%sv = load %s, %s* %s, align 4' % (t, name, HANDLE_TYPE, HANDLE_TYPE, glob),
                '  %s.%sl = call %s @dx.op.createHandleForLib.dx.types.Handle(i32 160, %s %s.%sv)'
                '  ; CreateHandleForLib(Resource)' % (t, name, HANDLE_TYPE, HANDLE_TYPE, t, name),
                '  %s.%sh = call %s @dx.op.annotateHandle(i32 216, %s %s.%sl, '
                '%s { i32 11, i32 0 })  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer'
                % (t, name, HANDLE_TYPE, HANDLE_TYPE, t, name, RESPROPS),
            ]
        else:
            o += [
                '  %s.%sv = load %s, %s* %s, align 4' % (t, name, BAB, BAB, glob),
                '  %s.%sh = call %s @dx.op.createHandleForLib.struct.ByteAddressBuffer(i32 160, %s %s.%sv)'
                '  ; CreateHandleForLib(Resource)' % (t, name, HANDLE_TYPE, BAB, t, name),
            ]

    def raw(o, v, h, off):
        o += [
            '  %s.r = call %s @dx.op.rawBufferLoad.i32(i32 139, %s %s, i32 %s, i32 undef, '
            'i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)'
            % (v, RESRET, HANDLE_TYPE, h, off),
            '  %s = extractvalue %s %s.r, 0' % (v, RESRET, v),
        ]

    def tlas_handle(o, t, sfx, c, p=0):
        if sm66:
            o += [
                '  %s.v%s = load %s, %s* %s, align 4' % (t, sfx, HANDLE_TYPE, HANDLE_TYPE, _glob(slot[0], c, p)),
                '  %s.l%s = call %s @dx.op.createHandleForLib.dx.types.Handle(i32 160, %s %s.v%s)'
                '  ; CreateHandleForLib(Resource)' % (t, sfx, HANDLE_TYPE, HANDLE_TYPE, t, sfx),
                '  %s.h%s = call %s @dx.op.annotateHandle(i32 216, %s %s.l%s, '
                '%s { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure'
                % (t, sfx, HANDLE_TYPE, HANDLE_TYPE, t, sfx, RESPROPS),
            ]
        else:
            o += [
                '  %s.v%s = load %s, %s* %s, align 4' % (t, sfx, RAS, RAS, _glob(slot[0], c, p)),
                '  %s.h%s = call %s @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure'
                '(i32 160, %s %s.v%s)  ; CreateHandleForLib(Resource)' % (t, sfx, HANDLE_TYPE, RAS, t, sfx),
            ]

    def lookup(o, t, args, key, head, mid, cap):
        """Slot slot[0]'s sub-slot for `key`, from the shim's key table, then
        a switch over (sub-slot, structure) tracing each."""
        lab = t[1:]
        o += ['  br label %%%s.Lp' % lab, '', '%s.Lp:' % lab]
        bab_handle(o, t, 'y', KEYS_GLOBAL, None)
        raw(o, t + '.yb', t + '.yh', '%d' % (4 * slot[0]))
        o.append('  %s.ya = shl i32 %s.yb, 2' % (t, t))
        raw(o, t + '.yn', t + '.yh', t + '.ya')
        o += [
            '  br label %%%s.Lh' % lab, '', '%s.Lh:' % lab,
            '  %s.ylo = phi i32 [ 0, %%%s.Lp ], [ %s.ylo2, %%%s.Lb ]' % (t, lab, t, lab),
            '  %s.yhi = phi i32 [ %s.yn, %%%s.Lp ], [ %s.yhi2, %%%s.Lb ]' % (t, t, lab, t, lab),
            '  %s.yc = icmp ult i32 %s.ylo, %s.yhi' % (t, t, t),
            '  br i1 %s.yc, label %%%s.Lb, label %%%s.Lx' % (t, lab, lab),
            '', '%s.Lb:' % lab,
            '  %s.ys = add i32 %s.ylo, %s.yhi' % (t, t, t),
            '  %s.ym = lshr i32 %s.ys, 1' % (t, t),
            '  %s.y2 = shl i32 %s.ym, 1' % (t, t),
            '  %s.y3 = add i32 %s.yb, %s.y2' % (t, t, t),
            '  %s.y4 = add i32 %s.y3, 1' % (t, t),
            '  %s.y5 = shl i32 %s.y4, 2' % (t, t),
        ]
        raw(o, t + '.yk', t + '.yh', t + '.y5')
        o += [
            '  %s.ylt = icmp ult i32 %s.yk, %s' % (t, t, key),
            '  %s.ym1 = add i32 %s.ym, 1' % (t, t),
            '  %s.ylo2 = select i1 %s.ylt, i32 %s.ym1, i32 %s.ylo' % (t, t, t, t),
            '  %s.yhi2 = select i1 %s.ylt, i32 %s.yhi, i32 %s.ym' % (t, t, t, t),
            '  br label %%%s.Lh' % lab,
            '', '%s.Lx:' % lab,
            '  %s.yf1 = shl i32 %s.ylo, 1' % (t, t),
            '  %s.yf2 = add i32 %s.yb, %s.yf1' % (t, t, t),
            '  %s.yf3 = add i32 %s.yf2, 1' % (t, t),
            '  %s.yf4 = shl i32 %s.yf3, 2' % (t, t),
        ]
        raw(o, t + '.yfk', t + '.yh', t + '.yf4')
        o.append('  %s.yf5 = add i32 %s.yf4, 4' % (t, t))
        raw(o, t + '.yfp', t + '.yh', t + '.yf5')
        o += [
            '  %s.yf6 = icmp eq i32 %s.yfk, %s' % (t, t, key),
            '  %s.yf7 = icmp ult i32 %s.ylo, %s.yn' % (t, t, t),
            '  %s.yf8 = and i1 %s.yf6, %s.yf7' % (t, t, t),
            '  %s.p = select i1 %s.yf8, i32 %s.yfp, i32 0' % (t, t, t),
        ]
        u = t + '.p'
        if cc > 1:
            o += [
                '  %s.c = lshr i32 %s.e, 16' % (t, t),
                '  %s.pc = mul i32 %s.p, %d' % (t, t, cc),
                '  %s.u = add i32 %s.pc, %s.c' % (t, t, t),
            ]
            u = t + '.u'
        o.append('  switch i32 %s, label %%%s.b0 [' % (u, lab))
        for x in range(1, cap * cc):
            o.append('    i32 %d, label %%%s.b%d' % (x, lab, x))
        o.append('  ]')
        for x in range(cap * cc):
            o += ['', '%s.b%d:' % (lab, x)]
            tlas_handle(o, t, str(x), x % cc, x // cc)
            o.append('%s%s.h%d%s%s' % (head, t, x, mid, args))
            o.append('  br label %%%s.j' % lab)
        o += ['', '%s.j:' % lab]

    out, n = [], 0
    # Splitting a block moves its way out to the new join block: a phi naming
    # the block as a predecessor then names the join (per function).
    chain, renamed, fn_start = '', {}, 0
    for line in lines:
        if line.startswith('define '):
            chain, renamed, fn_start = '', {}, len(out)
            fn[0] += 1
        else:
            lm = _LABEL.match(line)
            if lm:
                chain = lm.group(1)
            elif line == '}' and renamed:
                for i in range(fn_start, len(out)):
                    if ' = phi ' not in out[i]:
                        continue
                    for a2, b2 in sorted(renamed.items()):
                        out[i] = out[i].replace(', %%%s ]' % a2, ', %%%s ]' % b2)
        m = _TRACE.match(line)
        if not m:
            out.append(line)
            continue
        if slot_of and n >= len(slot_of):
            raise Unsupported('more TraceRay calls than scene slots given')
        slot[0] = slot_of[n] if slot_of else 0
        if slot[0] >= slots:
            raise Unsupported("a TraceRay's scene slot is out of range")
        R, M = m.group(6), m.group(7)
        t = '%%st.%d' % n
        head = '%scall void @dx.op.traceRay.%s(i32 157, %s ' % (m.group(1), m.group(2), HANDLE_TYPE)
        mid = ', i32 %s, i32 %s, i32 ' % (m.group(4), m.group(5))
        tail = m.group(8)
        cap = caps[slot[0]]
        key = None
        if cap > 1:
            key = _key_of(defs[fn[0]] if 0 <= fn[0] < len(defs) else {}, m.group(3))
            if key is None:
                raise Unsupported('the key a TraceRay picks its scene by could not be found')
        if not copies:
            if not re.match(r'^-?\d+$', R) or not re.match(r'^-?\d+$', M):
                raise Unsupported('a TraceRay whose hit group arguments are computed at run time')
            pair = (int(R) & 15, int(M) & 15)
            if pair not in pairs:
                raise Unsupported('a TraceRay argument pair the pipeline did not list')
            if cap == 1:
                tlas_handle(out, t, '', 0)
                out.append('%s%s.h%s%d, i32 %d, %s' % (head, t, mid, pairs.index(pair), k, tail))
                n += 1
                continue
            lookup(out, t, '%d, i32 %d, %s' % (pairs.index(pair), k, tail), key, head, mid, cap)
            if chain:
                renamed[chain] = 'st.%d.j' % n
            n += 1
            continue
        out += [
            '  %s.r = and i32 %s, 15' % (t, R),
            '  %s.m = and i32 %s, 15' % (t, M),
            '  %s.s = shl i32 %s.r, 4' % (t, t),
            '  %s.k = or i32 %s.s, %s.m' % (t, t, t),
            '  %s.o = shl i32 %s.k, 2' % (t, t),
        ]
        if sm66:
            out += [
                '  %s.lv = load %s, %s* %s, align 4' % (t, HANDLE_TYPE, HANDLE_TYPE, LUT_GLOBAL),
                '  %s.ll = call %s @dx.op.createHandleForLib.dx.types.Handle(i32 160, %s %s.lv)'
                '  ; CreateHandleForLib(Resource)' % (t, HANDLE_TYPE, HANDLE_TYPE, t),
                '  %s.lh = call %s @dx.op.annotateHandle(i32 216, %s %s.ll, '
                '%s { i32 11, i32 0 })  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer'
                % (t, HANDLE_TYPE, HANDLE_TYPE, t, RESPROPS),
            ]
        else:
            out += [
                '  %s.lv = load %s, %s* %s, align 4' % (t, BAB, BAB, LUT_GLOBAL),
                '  %s.lh = call %s @dx.op.createHandleForLib.struct.ByteAddressBuffer(i32 160, %s %s.lv)'
                '  ; CreateHandleForLib(Resource)' % (t, HANDLE_TYPE, BAB, t),
            ]
        out += [
            '  %s.lr = call %s @dx.op.rawBufferLoad.i32(i32 139, %s %s.lh, i32 %s.o, i32 undef, '
            'i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)'
            % (t, RESRET, HANDLE_TYPE, t, t),
            '  %s.e = extractvalue %s %s.lr, 0' % (t, RESRET, t),
            '  %s.q = and i32 %s.e, 255' % (t, t),
            '  %s.x = lshr i32 %s.e, 8' % (t, t),
            '  %s.kk = and i32 %s.x, 255' % (t, t),
        ]
        args = '%s%s.q, i32 %s.kk, %s' % (mid, t, t, tail)
        if cap > 1:
            lookup(out, t, '%s.q, i32 %s.kk, %s' % (t, t, tail), key, head, mid, cap)
            if chain:
                renamed[chain] = 'st.%d.j' % n
            n += 1
            continue
        if copies == 1:
            tlas_handle(out, t, '', 0)
            out.append('%s%s.h%s' % (head, t, args))
            n += 1
            continue
        lab = 'st.%d' % n
        out.append('  %s.c = lshr i32 %s.e, 16' % (t, t))
        out.append('  switch i32 %s.c, label %%%s.b0 [' % (t, lab))
        for c in range(1, copies):
            out.append('    i32 %d, label %%%s.b%d' % (c, lab, c))
        out.append('  ]')
        for c in range(copies):
            out += ['', '%s.b%d:' % (lab, c)]
            tlas_handle(out, t, str(c), c)
            out.append('%s%s.h%d%s' % (head, t, c, args))
            out.append('  br label %%%s.j' % lab)
        out += ['', '%s.j:' % lab]
        if chain:
            renamed[chain] = lab + '.j'
        n += 1
    lines = out

    body = '\n'.join(lines)
    decls = []

    def declare(name, d):
        if name not in body:
            decls.append(d)

    if sm66:
        declare('declare %s @dx.op.createHandleForLib.dx.types.Handle(' % HANDLE_TYPE,
                'declare %s @dx.op.createHandleForLib.dx.types.Handle(i32, %s) #RQRO'
                % (HANDLE_TYPE, HANDLE_TYPE))
        declare('declare %s @dx.op.annotateHandle(' % HANDLE_TYPE,
                'declare %s @dx.op.annotateHandle(i32, %s, %s) #RQNONE'
                % (HANDLE_TYPE, HANDLE_TYPE, RESPROPS))
    else:
        declare('declare %s @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure('
                % HANDLE_TYPE,
                'declare %s @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure'
                '(i32, %s) #RQRO' % (HANDLE_TYPE, RAS))
        if copies or keyed:
            declare('declare %s @dx.op.createHandleForLib.struct.ByteAddressBuffer(' % HANDLE_TYPE,
                    'declare %s @dx.op.createHandleForLib.struct.ByteAddressBuffer(i32, %s) #RQRO'
                    % (HANDLE_TYPE, BAB))
    if copies or keyed:
        declare('declare %s @dx.op.rawBufferLoad.i32(' % RESRET,
                'declare %s @dx.op.rawBufferLoad.i32(i32, %s, i32, i32, i8, i32) #RQRO'
                % (RESRET, HANDLE_TYPE))
    last_decl = max(k2 for k2, l in enumerate(lines) if l.startswith('declare '))
    ins = []
    for d in decls:
        ins += ['', d]
    lines[last_decl + 1:last_decl + 1] = ins

    types = []
    if not any(l.startswith('%s = type' % HANDLE_TYPE) for l in lines):
        types.append('%s = type { i8* }' % HANDLE_TYPE)
    if sm66 and not any(l.startswith('%s = type' % RESPROPS) for l in lines):
        types.append('%s = type { i32, i32 }' % RESPROPS)
    if not any(l.startswith('%s = type' % RAS) for l in lines):
        types.append('%s = type { i32 }' % RAS)
    if (copies or keyed) and not any(l.startswith('%s = type' % BAB) for l in lines):
        types.append('%s = type { i32 }' % BAB)
    if (copies or keyed) and not any(l.startswith('%s = type' % RESRET) for l in lines):
        types.append('%s = type { i32, i32, i32, i32, i32 }' % RESRET)
    last_type = max(k2 for k2, l in enumerate(lines) if re.match(r'^%[\w.]+ = type ', l))
    lines[last_type + 1:last_type + 1] = types
    last_global = max((k2 for k2, l in enumerate(lines) if re.match(r'^@[\w."\\?@]+ = ', l)),
                      default=-1)
    at = (last_global if last_global >= 0 else last_type + len(types)) + 1
    g = [''] if last_global < 0 else []
    for j in range(slots):
        for q in range(caps[j]):
            for c in range(copies or 1):
                g.append('%s = external constant %s, align 4' % (_glob(j, c, q), HANDLE_TYPE if sm66 else RAS))
    if copies:
        g.append('%s = external constant %s, align 4' % (LUT_GLOBAL, HANDLE_TYPE if sm66 else BAB))
    if keyed:
        g.append('%s = external constant %s, align 4' % (KEYS_GLOBAL, HANDLE_TYPE if sm66 else BAB))
    lines[at:at] = g
    text = _metadata('\n'.join(lines), sm66, copies, slots, caps)
    return _resolve_attrs(text), n


def _glob(j, c, p=0):
    b = TLAS_GLOBAL + ('.s%d' % j if j else '') + ('.k%d' % p if p else '')
    return b + ('.%d' % c if c else '')


def _metadata(text, sm66, copies, slots=1, caps=None):
    md = {}
    for m in re.finditer(r'^!(\d+) = (?:distinct )?!\{(.*)\}\s*$', text, re.M):
        md[int(m.group(1))] = m.group(2)
    nxt = [max(md) + 1 if md else 0]
    added = []

    def node(body):
        nid = nxt[0]
        nxt[0] += 1
        added.append('!%d = !{%s}' % (nid, body))
        return nid

    def rewrite(nid, body):
        pat = re.compile(r'^!%d = !\{.*\}\s*$' % nid)
        return '\n'.join('!%d = !{%s}' % (nid, body) if pat.match(l) else l
                         for l in text.split('\n'))

    def ref(ty, gl):
        return ('%s* bitcast (%s* %s to %s*)' % (ty, HANDLE_TYPE, gl, ty)
                if sm66 else '%s* %s' % (ty, gl))

    ep = re.search(r'^!dx\.entryPoints = !\{(.*)\}\s*$', text, re.M)
    if not ep:
        raise Unsupported('no !dx.entryPoints')
    entry = None
    for r in ep.group(1).split(','):
        nid = int(r.strip()[1:])
        if md[nid].split(',')[0].strip() == 'null':
            entry = nid
    if entry is None:
        raise Unsupported('no module entry in !dx.entryPoints')
    fields = [f.strip() for f in md[entry].split(',')]

    extra = node('i32 0, i32 4')
    res = re.search(r'^!dx\.resources = !\{!(\d+)\}\s*$', text, re.M)
    have, groups = [], []
    if res:
        rid = int(res.group(1))
        groups = [g.strip() for g in md[rid].split(',')]
        if groups[0] != 'null':
            have = [x.strip() for x in md[int(groups[0][1:])].split(',')]
    first = len(have)
    cc = copies or 1
    caps = caps or [1] * slots
    u = 0
    for j in range(slots):
        for q in range(caps[j]):
            for c in range(cc):
                gl = _glob(j, c, q)
                rec = node('i32 %d, %s, !"%s", i32 %d, i32 %d, i32 1, i32 16, i32 0, !%d'
                           % (first + u * cc + c, ref(RAS, gl), gl[1:], GI_SPACE, u * cc + c, extra))
                have.append('!%d' % rec)
            u += 1
    if copies:
        rec = node('i32 %d, %s, !"%s", i32 %d, i32 %d, i32 1, i32 11, i32 0, null'
                   % (first + u * cc, ref(BAB, LUT_GLOBAL), LUT_GLOBAL[1:], GI_SPACE,
                      u * cc))
        have.append('!%d' % rec)
    if u > slots:
        r = u * cc + (1 if copies else 0)
        rec = node('i32 %d, %s, !"%s", i32 %d, i32 %d, i32 1, i32 11, i32 0, null'
                   % (first + r, ref(BAB, KEYS_GLOBAL), KEYS_GLOBAL[1:], GI_SPACE, r))
        have.append('!%d' % rec)
    if res:
        groups[0] = '!%d' % node(', '.join(have))
        text = rewrite(rid, ', '.join(groups))
    else:
        lst = node(', '.join(have))
        rid = node('!%d, null, null, null' % lst)
        ls = text.split('\n')
        at = next(k for k, l in enumerate(ls) if l.startswith('!dx.entryPoints = '))
        ls.insert(at, '!dx.resources = !{!%d}' % rid)
        text = '\n'.join(ls)
        fields[3] = '!%d' % rid
        text = rewrite(entry, ', '.join(fields))
    return text.rstrip('\n') + '\n' + '\n'.join(added) + '\n'


def main(argv):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'hand'))
    from llnorm import normalize
    rest, copies, slots, slot_of, caps = argv[3:], 0, 1, None, None
    if rest[:1] == ['--copies']:
        copies, rest = int(rest[1]), rest[2:]
    if '--caps' in rest:
        i = rest.index('--caps')
        caps = [int(x) for x in rest[i + 1].split(',')]
        rest = rest[:i] + rest[i + 2:]
    if '--slots' in rest:
        i = rest.index('--slots')
        slots, slot_of = int(rest[i + 1]), [int(x) for x in rest[i + 2].split(',')]
        rest = rest[:i] + rest[i + 3:]
    pairs = [tuple(int(x) for x in a.split(',')) for a in rest]
    text = normalize(io.open(argv[1], encoding='utf-8').read())
    try:
        out, calls = retrace(text, pairs, copies, slot_of, slots, caps)
    except Unsupported as e:
        print('UNSUPPORTED: %s' % e)
        return 2
    io.open(argv[2], 'w', encoding='utf-8', newline='\n').write(out)
    print('TraceRay calls pointed at the shim scene: %d' % calls)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
