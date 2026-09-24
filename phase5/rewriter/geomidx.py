"""GeometryIndex() in an APPLICATION's own DXR 1.0 hit shaders.

GeometryIndex() (dx.op 213) is Tier 1.1: it sets the library's module shader
flag 0x2000000 (SFI0 bit 20, the same bit RayQuery sets), and the GTX 1070's
driver fails the whole CreateStateObject. A DXR 1.0 hit shader has no other
way to learn which geometry it serves: only WHICH RECORD it runs from differs.

So every call becomes a read of one 32-bit constant from a cbuffer at
b0 in a register space no application uses (GI_SPACE). The shim appends that
constant to the local root signature of every hit group whose shaders read
it, and writes the geometry index into each record of its own copy of the
application's shader table. Nothing else in the module moves.

Shapes copied from DXC, phase5/cases/reference/lib_shimgeom_ref.hlsl:

    lib_6_5  @G = external constant %T;  load %T, createHandleForLib.T (160)
    lib_6_6  @G = external constant %dx.types.Handle; load, createHandleForLib
             .dx.types.Handle (160), annotateHandle (216) as a CBuffer {13, 4}

then cbufferLoadLegacy (59) register 0, element 0. The resource record is a
CBV {id, global, name, space, lower bound 0, range 1, size 4, null}.

The module flag 0x2000000 is cleared, since nothing Tier 1.1 is left. A
library that ALSO uses RayQuery is refused: that is a different item (RayQuery
inside DXR shaders), and clearing the flag would be a lie about it.

    python phase5/rewriter/geomidx.py <in.ll> <out.ll>
"""

import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lower import _resolve_attrs, _is_sm66, HANDLE_TYPE

GEOMETRY_INDEX = 213
TIER11_FLAG = 0x2000000
# The register space of the shim's constant: b0, space GI_SPACE. High enough
# that no engine binds it; a module already using it is refused.
GI_SPACE = 0x7FFF0000
GI_TYPE = '%dxr11.gi'
GI_GLOBAL = '@dxr11.gi'
GI_NAME = 'dxr11.gi'
CBUFRET = '%dx.types.CBufRet.i32'
RESPROPS = '%dx.types.ResourceProperties'


class Unsupported(Exception):
    pass


_CALL = re.compile(r'^(\s*)(%[\w.]+) = call i32 @dx\.op\.geometryIndex\.i32\(i32 213\)')


def _read(sm66, n, result):
    t = '%%gi.%d' % n
    if sm66:
        return [
            '  %s.v = load %s, %s* %s, align 4' % (t, HANDLE_TYPE, HANDLE_TYPE, GI_GLOBAL),
            '  %s.l = call %s @dx.op.createHandleForLib.dx.types.Handle(i32 160, %s %s.v)'
            '  ; CreateHandleForLib(Resource)' % (t, HANDLE_TYPE, HANDLE_TYPE, t),
            '  %s.h = call %s @dx.op.annotateHandle(i32 216, %s %s.l, '
            '%s { i32 13, i32 4 })  ; AnnotateHandle(res,props)  resource: CBuffer'
            % (t, HANDLE_TYPE, HANDLE_TYPE, t, RESPROPS),
            '  %s.r = call %s @dx.op.cbufferLoadLegacy.i32(i32 59, %s %s.h, i32 0)'
            '  ; CBufferLoadLegacy(handle,regIndex)' % (t, CBUFRET, HANDLE_TYPE, t),
            '  %s = extractvalue %s %s.r, 0' % (result, CBUFRET, t),
        ]
    return [
        '  %s.v = load %s, %s* %s, align 4' % (t, GI_TYPE, GI_TYPE, GI_GLOBAL),
        '  %s.h = call %s @dx.op.createHandleForLib.%s(i32 160, %s %s.v)'
        '  ; CreateHandleForLib(Resource)' % (t, HANDLE_TYPE, GI_NAME, GI_TYPE, t),
        '  %s.r = call %s @dx.op.cbufferLoadLegacy.i32(i32 59, %s %s.h, i32 0)'
        '  ; CBufferLoadLegacy(handle,regIndex)' % (t, CBUFRET, HANDLE_TYPE, t),
        '  %s = extractvalue %s %s.r, 0' % (result, CBUFRET, t),
    ]


def export_name(fn):
    """The name a state object uses for a library function: DXC mangles
    `\\01?CH@@YAX...` and exports it as CH."""
    m = re.match(r'^\\01\?([^@]+)@@', fn)
    return m.group(1) if m else fn


def lower(text):
    """Returns (text, functions). functions lists every function that read
    GeometryIndex(), by the name its library exports it under. An input with
    no GeometryIndex() comes back unchanged with no functions."""
    lines = text.split('\n')
    if not any(_CALL.match(l) for l in lines):
        return text, []
    if re.search(r'@dx\.op\.rayQuery_|@dx\.op\.allocateRayQuery', text):
        raise Unsupported('the library reads GeometryIndex() and also uses RayQuery; '
                          'RayQuery inside DXR shaders has no lowering yet')
    if GI_GLOBAL + ' ' in text or GI_TYPE + ' ' in text:
        raise Unsupported('the library already defines %s' % GI_GLOBAL)
    if re.search(r'i32 %d, i32 \d+, i32 \d+, i32 \d+' % GI_SPACE, text):
        raise Unsupported('the library already binds register space %d' % GI_SPACE)
    sm66 = _is_sm66(text)

    out, functions, fn, n = [], [], None, 0
    for line in lines:
        m = re.match(r'^define [^@]*@"?([^"(]+)"?\(', line)
        if m:
            fn = m.group(1)
        c = _CALL.match(line)
        if c:
            out += _read(sm66, n, c.group(2))
            n += 1
            name = export_name(fn)
            if name not in functions:
                functions.append(name)
            continue
        out.append(line)
    lines = out

    # Declarations: drop GeometryIndex's, add what the reads call, each only
    # if the module does not declare it already (twice is an error, and so is
    # a declare nothing calls).
    out = []
    for k, line in enumerate(lines):
        if line.startswith('declare i32 @dx.op.geometryIndex.i32('):
            if out and out[-1].startswith('; Function Attrs:'):
                out.pop()
            if out and out[-1] == '' and k + 1 < len(lines) and lines[k + 1] == '':
                out.pop()
            continue
        out.append(line)
    lines = out
    body = '\n'.join(lines)
    decls = []
    if 'declare %s @dx.op.cbufferLoadLegacy.i32(' % CBUFRET not in body:
        decls.append('declare %s @dx.op.cbufferLoadLegacy.i32(i32, %s, i32) #RQRO'
                     % (CBUFRET, HANDLE_TYPE))
    if sm66:
        if 'declare %s @dx.op.createHandleForLib.dx.types.Handle(' % HANDLE_TYPE not in body:
            decls.append('declare %s @dx.op.createHandleForLib.dx.types.Handle(i32, %s) #RQRO'
                         % (HANDLE_TYPE, HANDLE_TYPE))
        if 'declare %s @dx.op.annotateHandle(' % HANDLE_TYPE not in body:
            decls.append('declare %s @dx.op.annotateHandle(i32, %s, %s) #RQNONE'
                         % (HANDLE_TYPE, HANDLE_TYPE, RESPROPS))
    else:
        decls.append('declare %s @dx.op.createHandleForLib.%s(i32, %s) #RQRO'
                     % (HANDLE_TYPE, GI_NAME, GI_TYPE))
    # After the last declaration, or after the last function when the one
    # just removed was the only one.
    at_decl = [k for k, l in enumerate(lines) if l.startswith('declare ')]
    last_decl = at_decl[-1] if at_decl else max(k for k, l in enumerate(lines) if l == '}')
    ins = []
    for d in decls:
        ins += ['', d]
    lines[last_decl + 1:last_decl + 1] = ins

    # Types and the global, after the last type definition.
    types = []
    if not any(l.startswith('%s = type' % HANDLE_TYPE) for l in lines):
        types.append('%s = type { i8* }' % HANDLE_TYPE)
    if not any(l.startswith('%s = type' % CBUFRET) for l in lines):
        types.append('%s = type { i32, i32, i32, i32 }' % CBUFRET)
    if sm66 and not any(l.startswith('%s = type' % RESPROPS) for l in lines):
        types.append('%s = type { i32, i32 }' % RESPROPS)
    types.append('%s = type { i32 }' % GI_TYPE)
    last_type = max(k for k, l in enumerate(lines) if re.match(r'^%[\w.]+ = type ', l))
    lines[last_type + 1:last_type + 1] = types
    last_global = max((k for k, l in enumerate(lines) if re.match(r'^@[\w.]+ = ', l)), default=-1)
    at = (last_global if last_global >= 0 else last_type + len(types)) + 1
    lines[at:at] = ([''] if last_global < 0 else []) + [
        '%s = external constant %s' % (GI_GLOBAL, HANDLE_TYPE if sm66 else GI_TYPE)]
    text = '\n'.join(lines)
    text = _metadata(text, sm66)
    return _resolve_attrs(text), functions


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
        # On lines: a multiline \s*$ also eats the newlines after it.
        pat = re.compile(r'^!%d = !\{.*\}\s*$' % nid)
        return '\n'.join('!%d = !{%s}' % (nid, body) if pat.match(l) else l
                         for l in text.split('\n'))

    gref = ('%s* bitcast (%s* %s to %s*)' % (GI_TYPE, HANDLE_TYPE, GI_GLOBAL, GI_TYPE)
            if sm66 else '%s* %s' % (GI_TYPE, GI_GLOBAL))

    # The module entry: the one in !dx.entryPoints whose function is null.
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

    res = re.search(r'^!dx\.resources = !\{!(\d+)\}\s*$', text, re.M)
    if res:
        rid = int(res.group(1))
        groups = [g.strip() for g in md[rid].split(',')]
        have = []
        if groups[2] != 'null':
            have = [x.strip() for x in md[int(groups[2][1:])].split(',')]
        rec = node('i32 %d, %s, !"%s", i32 %d, i32 0, i32 1, i32 4, null'
                   % (len(have), gref, GI_NAME, GI_SPACE))
        groups[2] = '!%d' % node(', '.join(have + ['!%d' % rec]))
        text = rewrite(rid, ', '.join(groups))
    else:
        rec = node('i32 0, %s, !"%s", i32 %d, i32 0, i32 1, i32 4, null'
                   % (gref, GI_NAME, GI_SPACE))
        rid = node('null, null, !%d, null' % node('!%d' % rec))
        ls = text.split('\n')
        at = next(k for k, l in enumerate(ls) if l.startswith('!dx.entryPoints = '))
        ls.insert(at, '!dx.resources = !{!%d}' % rid)
        text = '\n'.join(ls)
        fields[3] = '!%d' % rid

    # Clear the Tier 1.1 flag in the module entry's properties.
    if fields[4] != 'null':
        pid = int(fields[4][1:])
        props = [p.strip() for p in md[pid].split(',')]
        for k in range(0, len(props) - 1, 2):
            if props[k] == 'i32 0':
                flags = int(props[k + 1].split()[1]) & ~TIER11_FLAG
                if flags:
                    props[k + 1] = 'i64 %d' % flags
                else:
                    del props[k:k + 2]
                break
        if props:
            text = rewrite(pid, ', '.join(props))
        else:
            fields[4] = 'null'
    text = rewrite(entry, ', '.join(fields))
    return text.rstrip('\n') + '\n' + '\n'.join(added) + '\n'


def main(argv):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'hand'))
    from llnorm import normalize
    text = normalize(io.open(argv[1], encoding='utf-8').read())
    try:
        out, functions = lower(text)
    except Unsupported as e:
        print('UNSUPPORTED: %s' % e)
        return 2
    io.open(argv[2], 'w', encoding='utf-8', newline='\n').write(out)
    print('GeometryIndex() read in: %s' % (', '.join(functions) or 'nothing'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
