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
RAS = '%struct.RaytracingAccelerationStructure'
RESPROPS = '%dx.types.ResourceProperties'

_TRACE = re.compile(r'^(\s*)call void @dx\.op\.traceRay\.([^(]+)\(i32 157, '
                    r'%dx\.types\.Handle (%[\w.]+), i32 ([^,]+), i32 ([^,]+), '
                    r'i32 ([^,]+), i32 ([^,]+), (.*)$')


def retrace(text, pairs):
    """Returns (text, calls). `pairs` is the pipeline's list of (R, M), low 4
    bits each; a call's r is the index of its pair. No TraceRay: unchanged."""
    lines = text.split('\n')
    if not any(_TRACE.match(l) for l in lines):
        return text, 0
    if TLAS_GLOBAL + ' ' in text:
        raise Unsupported('the library already defines %s' % TLAS_GLOBAL)
    sm66 = _is_sm66(text)
    k = len(pairs)
    out, n = [], 0
    for line in lines:
        m = _TRACE.match(line)
        if not m:
            out.append(line)
            continue
        R, M = m.group(6), m.group(7)
        if not re.match(r'^-?\d+$', R) or not re.match(r'^-?\d+$', M):
            raise Unsupported('a TraceRay whose hit group arguments are computed at run time')
        pair = (int(R) & 15, int(M) & 15)
        if pair not in pairs:
            raise Unsupported('a TraceRay argument pair the pipeline did not list')
        t = '%%st.%d' % n
        if sm66:
            out += [
                '  %s.v = load %s, %s* %s, align 4' % (t, HANDLE_TYPE, HANDLE_TYPE, TLAS_GLOBAL),
                '  %s.l = call %s @dx.op.createHandleForLib.dx.types.Handle(i32 160, %s %s.v)'
                '  ; CreateHandleForLib(Resource)' % (t, HANDLE_TYPE, HANDLE_TYPE, t),
                '  %s.h = call %s @dx.op.annotateHandle(i32 216, %s %s.l, '
                '%s { i32 16, i32 0 })  ; AnnotateHandle(res,props)  resource: RTAccelerationStructure'
                % (t, HANDLE_TYPE, HANDLE_TYPE, t, RESPROPS),
            ]
        else:
            out += [
                '  %s.v = load %s, %s* %s, align 4' % (t, RAS, RAS, TLAS_GLOBAL),
                '  %s.h = call %s @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure'
                '(i32 160, %s %s.v)  ; CreateHandleForLib(Resource)' % (t, HANDLE_TYPE, RAS, t),
            ]
        out.append('%scall void @dx.op.traceRay.%s(i32 157, %s %s.h, i32 %s, i32 %s, i32 %d, '
                   'i32 %d, %s'
                   % (m.group(1), m.group(2), HANDLE_TYPE, t, m.group(4), m.group(5),
                      pairs.index(pair), k, m.group(8)))
        n += 1
    lines = out

    body = '\n'.join(lines)
    decls = []
    if sm66:
        if 'declare %s @dx.op.createHandleForLib.dx.types.Handle(' % HANDLE_TYPE not in body:
            decls.append('declare %s @dx.op.createHandleForLib.dx.types.Handle(i32, %s) #RQRO'
                         % (HANDLE_TYPE, HANDLE_TYPE))
        if 'declare %s @dx.op.annotateHandle(' % HANDLE_TYPE not in body:
            decls.append('declare %s @dx.op.annotateHandle(i32, %s, %s) #RQNONE'
                         % (HANDLE_TYPE, HANDLE_TYPE, RESPROPS))
    elif ('declare %s @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure('
          % HANDLE_TYPE) not in body:
        decls.append('declare %s @dx.op.createHandleForLib.struct.RaytracingAccelerationStructure'
                     '(i32, %s) #RQRO' % (HANDLE_TYPE, RAS))
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
    last_type = max(k2 for k2, l in enumerate(lines) if re.match(r'^%[\w.]+ = type ', l))
    lines[last_type + 1:last_type + 1] = types
    last_global = max((k2 for k2, l in enumerate(lines) if re.match(r'^@[\w."\\?@]+ = ', l)),
                      default=-1)
    at = (last_global if last_global >= 0 else last_type + len(types)) + 1
    lines[at:at] = ([''] if last_global < 0 else []) + [
        '%s = external constant %s, align 4' % (TLAS_GLOBAL, HANDLE_TYPE if sm66 else RAS)]
    text = _metadata('\n'.join(lines), sm66)
    return _resolve_attrs(text), n


def _metadata(text, sm66):
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

    gref = ('%s* bitcast (%s* %s to %s*)' % (RAS, HANDLE_TYPE, TLAS_GLOBAL, RAS)
            if sm66 else '%s* %s' % (RAS, TLAS_GLOBAL))
    ep = re.search(r'^!dx\.entryPoints = !\{(.*)\}\s*$', text, re.M)
    if not ep:
        raise Unsupported('no !dx.entryPoints')
    entry = None
    for ref in ep.group(1).split(','):
        nid = int(ref.strip()[1:])
        if md[nid].split(',')[0].strip() == 'null':
            entry = nid
    if entry is None:
        raise Unsupported('no module entry in !dx.entryPoints')
    fields = [f.strip() for f in md[entry].split(',')]

    extra = node('i32 0, i32 4')
    res = re.search(r'^!dx\.resources = !\{!(\d+)\}\s*$', text, re.M)
    if res:
        rid = int(res.group(1))
        groups = [g.strip() for g in md[rid].split(',')]
        have = []
        if groups[0] != 'null':
            have = [x.strip() for x in md[int(groups[0][1:])].split(',')]
        rec = node('i32 %d, %s, !"%s", i32 %d, i32 0, i32 1, i32 16, i32 0, !%d'
                   % (len(have), gref, TLAS_NAME, GI_SPACE, extra))
        groups[0] = '!%d' % node(', '.join(have + ['!%d' % rec]))
        text = rewrite(rid, ', '.join(groups))
    else:
        rec = node('i32 0, %s, !"%s", i32 %d, i32 0, i32 1, i32 16, i32 0, !%d'
                   % (gref, TLAS_NAME, GI_SPACE, extra))
        rid = node('!%d, null, null, null' % node('!%d' % rec))
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
    pairs = [tuple(int(x) for x in a.split(',')) for a in argv[3:]]
    text = normalize(io.open(argv[1], encoding='utf-8').read())
    try:
        out, calls = retrace(text, pairs)
    except Unsupported as e:
        print('UNSUPPORTED: %s' % e)
        return 2
    io.open(argv[2], 'w', encoding='utf-8', newline='\n').write(out)
    print('TraceRay calls pointed at the shim scene: %d' % calls)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
