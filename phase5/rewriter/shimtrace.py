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
RAS = '%struct.RaytracingAccelerationStructure'
BAB = '%struct.ByteAddressBuffer'
RESRET = '%dx.types.ResRet.i32'
RESPROPS = '%dx.types.ResourceProperties'

_LABEL = re.compile(r'^([A-Za-z$._][\w$.]*):')
_TRACE = re.compile(r'^(\s*)call void @dx\.op\.traceRay\.([^(]+)\(i32 157, '
                    r'%dx\.types\.Handle (%[\w.]+), i32 ([^,]+), i32 ([^,]+), '
                    r'i32 ([^,]+), i32 ([^,]+), (.*)$')


def retrace(text, pairs, copies=0):
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
            or LUT_GLOBAL + ' ' in text):
        raise Unsupported('the library already defines %s' % TLAS_GLOBAL)
    sm66 = _is_sm66(text)
    k = len(pairs)

    def glob(c):
        return TLAS_GLOBAL + ('.%d' % c if c else '')

    def tlas_handle(o, t, sfx, c):
        if sm66:
            o += [
                '  %s.v%s = load %s, %s* %s, align 4' % (t, sfx, HANDLE_TYPE, HANDLE_TYPE, glob(c)),
                '  %s.l%s = call %s @dx.op.createHandleForLib.dx.types.Handle(i32 160, %s %s.v%s)'
                '  ; CreateHandleForLib(Resource)' % (t, sfx, HANDLE_TYPE, HANDLE_TYPE, t, sfx),
                '  %s.h%s = call %s @dx.op.annotateHandle(i32 216, %s %s.l%s, '
                '%s { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure'
                % (t, sfx, HANDLE_TYPE, HANDLE_TYPE, t, sfx, RESPROPS),
            ]
        else:
            o += [
                '  %s.v%s = load %s, %s* %s, align 4' % (t, sfx, RAS, RAS, glob(c)),
                '  %s.h%s = call %s @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure'
                '(i32 160, %s %s.v%s)  ; CreateHandleForLib(Resource)' % (t, sfx, HANDLE_TYPE, RAS, t, sfx),
            ]

    out, n = [], 0
    # Splitting a block moves its way out to the new join block: a phi naming
    # the block as a predecessor then names the join (per function).
    chain, renamed, fn_start = '', {}, 0
    for line in lines:
        if line.startswith('define '):
            chain, renamed, fn_start = '', {}, len(out)
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
        R, M = m.group(6), m.group(7)
        t = '%%st.%d' % n
        head = '%scall void @dx.op.traceRay.%s(i32 157, %s ' % (m.group(1), m.group(2), HANDLE_TYPE)
        mid = ', i32 %s, i32 %s, i32 ' % (m.group(4), m.group(5))
        tail = m.group(8)
        if not copies:
            if not re.match(r'^-?\d+$', R) or not re.match(r'^-?\d+$', M):
                raise Unsupported('a TraceRay whose hit group arguments are computed at run time')
            pair = (int(R) & 15, int(M) & 15)
            if pair not in pairs:
                raise Unsupported('a TraceRay argument pair the pipeline did not list')
            tlas_handle(out, t, '', 0)
            out.append('%s%s.h%s%d, i32 %d, %s' % (head, t, mid, pairs.index(pair), k, tail))
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
        if copies:
            declare('declare %s @dx.op.createHandleForLib.struct.ByteAddressBuffer(' % HANDLE_TYPE,
                    'declare %s @dx.op.createHandleForLib.struct.ByteAddressBuffer(i32, %s) #RQRO'
                    % (HANDLE_TYPE, BAB))
    if copies:
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
    if copies and not any(l.startswith('%s = type' % BAB) for l in lines):
        types.append('%s = type { i32 }' % BAB)
    if copies and not any(l.startswith('%s = type' % RESRET) for l in lines):
        types.append('%s = type { i32, i32, i32, i32, i32 }' % RESRET)
    last_type = max(k2 for k2, l in enumerate(lines) if re.match(r'^%[\w.]+ = type ', l))
    lines[last_type + 1:last_type + 1] = types
    last_global = max((k2 for k2, l in enumerate(lines) if re.match(r'^@[\w."\\?@]+ = ', l)),
                      default=-1)
    at = (last_global if last_global >= 0 else last_type + len(types)) + 1
    g = [''] if last_global < 0 else []
    for c in range(copies or 1):
        g.append('%s = external constant %s, align 4' % (glob(c), HANDLE_TYPE if sm66 else RAS))
    if copies:
        g.append('%s = external constant %s, align 4' % (LUT_GLOBAL, HANDLE_TYPE if sm66 else BAB))
    lines[at:at] = g
    text = _metadata('\n'.join(lines), sm66, copies)
    return _resolve_attrs(text), n


def _metadata(text, sm66, copies):
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
    for c in range(copies or 1):
        gl = TLAS_GLOBAL + ('.%d' % c if c else '')
        rec = node('i32 %d, %s, !"%s", i32 %d, i32 %d, i32 1, i32 16, i32 0, !%d'
                   % (first + c, ref(RAS, gl), gl[1:], GI_SPACE, c, extra))
        have.append('!%d' % rec)
    if copies:
        rec = node('i32 %d, %s, !"%s", i32 %d, i32 %d, i32 1, i32 11, i32 0, null'
                   % (first + copies, ref(BAB, LUT_GLOBAL), LUT_GLOBAL[1:], GI_SPACE, copies))
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
    rest, copies = argv[3:], 0
    if rest[:1] == ['--copies']:
        copies, rest = int(rest[1]), rest[2:]
    pairs = [tuple(int(x) for x in a.split(',')) for a in rest]
    text = normalize(io.open(argv[1], encoding='utf-8').read())
    try:
        out, calls = retrace(text, pairs, copies)
    except Unsupported as e:
        print('UNSUPPORTED: %s' % e)
        return 2
    io.open(argv[2], 'w', encoding='utf-8', newline='\n').write(out)
    print('TraceRay calls pointed at the shim scene: %d' % calls)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
