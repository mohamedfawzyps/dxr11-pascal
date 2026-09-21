#!/usr/bin/env python3
"""Lower an analysed RayQuery compute shader into a DXR library.

Every edit here is driven by the analysis, never by matching instruction text.
That is the whole difference from the hand lowerings in phase5/hand, which
work on exactly two files and nothing else.

The module is edited in place, as the recon concluded: the application's own
instructions are never moved between modules, so its bindings and handles
cannot be got wrong in transit. Lines nobody touches pass through byte for
byte.
"""

import re

from dxil import operand_name, render
import rayquery as rq

PAYLOAD = '%struct.Payload'
ATTRS = '%struct.BuiltInTriangleIntersectionAttributes'

# One payload shape carrying every committed accessor the whitelist supports.
# Tailoring it per shader would save a few bytes and cost a lot of ways to get
# the offsets wrong; the brief puts correctness well ahead of that.
#   0 float t | 1 <2 x float> bary | 2 i32 hit | 3 inst | 4 prim | 5 geom
PAYLOAD_TYPE = '{ float, <2 x float>, i32, i32, i32, i32 }'
PAYLOAD_BYTES = 28
PAYLOAD_FIELD = {
    rq.COMMITTED_RAY_T: (0, 'float', 8),
    rq.COMMITTED_BARY: (1, '<2 x float>', 4),
    rq.COMMITTED_STATUS: (2, 'i32', 4),
    rq.COMMITTED_INSTANCE_INDEX: (3, 'i32', 4),
    rq.COMMITTED_PRIMITIVE_INDEX: (4, 'i32', 4),
    rq.COMMITTED_GEOMETRY_INDEX: (5, 'i32', 4),
}
# Which dx.op gives each field in a DXR 1.0 closest-hit, read off DXC output.
CH_SOURCE = {
    3: ('i32', 'call i32 @dx.op.instanceIndex.i32(i32 142)'),
    4: ('i32', 'call i32 @dx.op.primitiveIndex.i32(i32 161)'),
    # Field 5, geometry index, is deliberately absent: see NO_LOWERING in
    # rayquery.py. Emitting dx.op.geometryIndex would set shader flag
    # 0x2000000 and the state object would then be refused by the driver.
}


class LowerError(Exception):
    pass


# --- resources -------------------------------------------------------------

_MD = re.compile(r'^!(\d+) = !\{(.*)\}\s*$', re.M)
_CLASS_NAMES = {0: 'srv', 1: 'uav', 2: 'cbv', 3: 'smp'}


def _metadata(text):
    return {int(m.group(1)): m.group(2) for m in _MD.finditer(text)}


_ARRAY = re.compile(r'^\[(\d+) x (.+)\]$')


def _resource_table(text, md):
    """{resource class -> [Res]} from !dx.resources, where Res is
    (node id, global type, element type, array length or None, name).

    A library's handles come from createHandleForLib applied to a loaded
    global, so every resource needs a real global and the record has to point
    at it instead of `undef`.

    A descriptor table does NOT by itself change any of this: measured, a
    table-bound shader produces a byte-identical function body and identical
    resource records, because the root signature lives outside the module.
    What a table enables and what does change the DXIL is a resource ARRAY,
    where the type is [N x T] and createHandle carries a real index."""
    m = re.search(r'!dx\.resources\s*=\s*!\{!(\d+)\}', text)
    if not m:
        raise LowerError('module has no !dx.resources')
    groups = [g.strip() for g in md[int(m.group(1))].split(',')]
    table = {}
    for cls, g in enumerate(groups):
        if g == 'null':
            continue
        ids = [int(x.strip()[1:]) for x in md[int(g[1:])].split(',')]
        recs = []
        for nid in ids:
            fields = [f.strip() for f in md[nid].split(',')]
            t = re.match(r'(\[[^\]]+\]|%[\w.$-]+|%"[^"]+")\*\s+\S+', fields[1])
            if not t:
                raise LowerError('resource record !%d has an unexpected shape' % nid)
            gty = t.group(1)
            arr = _ARRAY.match(gty)
            elem, count = (arr.group(2), int(arr.group(1))) if arr else (gty, None)
            name = re.match(r'!"([^"]*)"', fields[2])
            recs.append((nid, gty, elem, count, name.group(1) if name else ''))
        table[cls] = recs
    return table


def _handle_fn(llvm_type):
    """The createHandleForLib overload name for a resource type.

    DXC names it after the type with the leading % dropped, and quotes the
    whole symbol when the type does."""
    bare = llvm_type[1:]
    if bare.startswith('"'):
        return '"dx.op.createHandleForLib.%s"' % bare.strip('"'), bare.strip('"')
    return 'dx.op.createHandleForLib.%s' % bare, bare


# --- the lowering ----------------------------------------------------------

def lower(module, q, exports=None):
    """Return the text of a lib_6_5 module implementing `q` with TraceRay."""
    exports = exports or {'raygen': 'RayGen', 'anyhit': 'AnyHit',
                          'closesthit': 'ClosestHit', 'miss': 'Miss'}
    text = module.text
    md = _metadata(text)
    table = _resource_table(text, md)
    edits = {}

    globals_ = _plan_globals(table)
    _edit_resources(module, q, edits, table, globals_)
    _edit_ray_index(q, edits)
    _edit_query(module, q, edits, exports)

    fn = q.fn
    edits[fn.index] = 'define void @%s() #1 {' % exports['raygen']

    out = render(module, edits)
    out = _add_types_and_globals(out, globals_)
    out = _swap_declarations(out, q, globals_)
    out = _append_shaders(out, module, q, exports, table, globals_)
    out = _rewrite_metadata(out, md, table, globals_, q, exports)
    return out


def _plan_globals(table):
    """(class, node id) -> (global name, global type, element type, name)."""
    g = {}
    for cls, recs in table.items():
        for n, (nid, gty, elem, count, name) in enumerate(recs):
            sym = name if re.match(r'^[A-Za-z_]\w*$', name or '') \
                else 'rq_%s%d' % (_CLASS_NAMES.get(cls, 'res'), n)
            g[(cls, nid)] = (sym, gty, elem, name or sym)
    return g


def _handle_text(instr, table, globals_):
    """The load + createHandleForLib that replaces one createHandle.

    Shared, because the any-hit shader has to recreate any resource handle the
    Proceed loop body used. A handle is not caller state: it names a resource,
    and every shader in the library can reach it."""
    cls = int(re.match(r'i8\s+(\d+)', instr.args[1].strip()).group(1))
    rid = int(re.match(r'i32\s+(\d+)', instr.args[2].strip()).group(1))
    recs = table.get(cls, [])
    if rid >= len(recs):
        raise LowerError('createHandle names range %d of class %d, which '
                         '!dx.resources does not describe' % (rid, cls))
    nid = recs[rid][0]
    sym, gty, elem, _ = globals_[(cls, nid)]
    fnname, _ = _handle_fn(elem)
    raw = '%%rq.raw.%s' % instr.result[1:]

    if gty == elem:
        src = '{ty}* @{sym}'.format(ty=elem, sym=sym)
    else:
        # A resource array. DXC reaches the element through a constant
        # getelementptr on the array global, and hands createHandleForLib
        # the ELEMENT type. Matched against a DXC-built library.
        idx = re.match(r'i32\s+(\d+)$', instr.args[3].strip())
        if not idx:
            raise rq.Unsupported(
                'resource array indexed by a non-constant (%s); dynamic '
                'descriptor indexing is not supported'
                % instr.args[3].strip())
        nonuni = instr.args[4].strip()
        if nonuni not in ('i1 false', 'i1 0'):
            raise rq.Unsupported(
                'resource array indexed non-uniformly; not supported')
        src = ('{elem}* getelementptr inbounds ({gty}, {gty}* @{sym}, '
               'i32 0, i32 {i})').format(elem=elem, gty=gty, sym=sym,
                                         i=idx.group(1))

    return (
        '  {raw} = load {elem}, {src}, align 4\n'
        '  {res} = call %dx.types.Handle @{fn}(i32 160, {elem} {raw})'
        '  ; CreateHandleForLib(Resource)'
    ).format(raw=raw, elem=elem, src=src, res=instr.result, fn=fnname)


def _edit_resources(module, q, edits, table, globals_):
    """createHandle(57, class, rangeId, ...) -> load + createHandleForLib."""
    for block, instr in q.fn.instrs():
        if instr.dxop != 57:
            continue
        edits[instr.index] = _handle_text(instr, table, globals_)


def _edit_ray_index(q, edits):
    """ThreadId is not legal in a raygen; DispatchRaysIndex replaces it."""
    for block, instr in q.fn.instrs():
        if instr.dxop != 93:
            continue
        comp = re.match(r'i32\s+(\d+)', instr.args[1].strip()).group(1)
        edits[instr.index] = (
            '  %s = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 %s)'
            '  ; DispatchRaysIndex(col)' % (instr.result, comp))


def _edit_query(module, q, edits, exports):
    """Replace the query with a payload and one TraceRay, and drop the loop."""
    fn = q.fn

    # The payload alloca has to be in the entry block.
    entry = fn.blocks[0]
    if not entry.instrs:
        raise LowerError('entry block is empty')
    first = entry.instrs[0]
    pending = edits.get(first.index, first.line)
    edits[first.index] = ('  %%rq.pl = alloca %s, align 8\n%s' % (PAYLOAD, pending))

    # The allocate disappears entirely; the handle it produced is only ever
    # used by ops this pass rewrites.
    edits[q.alloc.index] = None

    ra = q.ray_args
    exit_label = None
    if q.loop:
        header, latch, body = q.loop
        exit_label = _loop_exit(fn, header, latch, body)
        _check_loop_isolated(fn, q, header, latch, body)

    edits[q.trace.index] = _trace_block(q, ra, exit_label)

    # Proceed calls vanish. The guard's branch is rewired past the loop; the
    # latch goes with the rest of the loop blocks.
    for block, instr in q.proceeds:
        edits[instr.index] = None
        if q.loop and block.label == q.loop[1]:
            continue
        term = block.terminator
        if term is not instr and term.index not in edits:
            m = re.match(r'\s*br\s+i1\s+(\S+),', term.line)
            if m and m.group(1) == instr.result:
                edits[term.index] = None   # folded into the trace replacement

    if q.loop:
        header, latch, body = q.loop
        preheader = _preheader(fn, q, header, body)
        for label in set(body) | ({preheader} if preheader else set()):
            blk = fn.block(label)
            if blk.index >= 0:
                edits[blk.index] = None
            for i in blk.instrs:
                edits[i.index] = None

    _edit_committed(q, edits)


def _trace_block(q, ra, exit_label):
    """Payload init plus the TraceRay that replaces the inline query."""
    flags = q.ray_flags
    lines = []
    for idx, ty, align in [(0, 'float', 8), (1, '<2 x float>', 4), (2, 'i32', 4),
                           (3, 'i32', 4), (4, 'i32', 4), (5, 'i32', 4)]:
        zero = '0.000000e+00' if ty == 'float' else (
            'zeroinitializer' if ty.startswith('<') else '0')
        lines.append('  %%rq.pl%d = getelementptr inbounds %s, %s* %%rq.pl, i32 0, i32 %d'
                     % (idx, PAYLOAD, PAYLOAD, idx))
        lines.append('  store %s %s, %s* %%rq.pl%d, align %d' % (ty, zero, ty, idx, align))
    lines += [
        '  call void @dx.op.traceRay.%s(i32 157, %%dx.types.Handle %s, i32 %d, '
        '%s, i32 0, i32 0, i32 0, %s, %s, %s, %s, %s, %s, %s, %s, %s* nonnull %%rq.pl)'
        '  ; TraceRay(...)' % (
            PAYLOAD[1:], q.as_handle, flags, ra['mask'],
            ra['origin'][0], ra['origin'][1], ra['origin'][2], ra['tmin'],
            ra['direction'][0], ra['direction'][1], ra['direction'][2], ra['tmax'],
            PAYLOAD),
    ]
    if exit_label:
        lines.append('  br label %s' % exit_label)
    return '\n'.join(lines)


def _loop_exit(fn, header, latch, body):
    outs = [s for s in fn.block(latch).successors() if s not in body]
    if len(outs) != 1:
        raise LowerError('Proceed loop has %d exits; expected one' % len(outs))
    return outs[0]


def _preheader(fn, q, header, body):
    """The block the guard branches through to reach the loop, if any."""
    guard = [b for b, _ in q.proceeds if b.label != q.loop[1]]
    if not guard:
        return None
    for s in guard[0].successors():
        if s not in body:
            blk = fn.block(s)
            if blk and blk.successors() == [header]:
                return s
    return None


def _check_loop_isolated(fn, q, header, latch, body):
    """The any-hit shader is a separate invocation with only the payload for
    shared state, so the loop body must not read caller locals or leak values
    back out. Both are in the brief's no-valid-lowering table."""
    defined, used = set(), set()
    for label in body:
        blk = fn.block(label)
        for i in blk.instrs:
            if i.result:
                defined.add(i.result)
            used.update(i.uses())

    # A resource handle read inside the loop is NOT caller state. It names a
    # resource, and the any-hit shader can create its own handle for the same
    # one, so those are recreated rather than refused. Real alpha testing reads
    # a texture or buffer in the loop body, so refusing this would have blocked
    # the most common shape there is.
    handles = {i.result for _, i in fn.instrs() if i.dxop == 57 and i.result}
    outside = {u for u in used
               if u not in defined and u != q.handle and u not in handles}
    if outside:
        raise rq.Unsupported(
            'Proceed loop body reads values defined outside it (%s); the '
            'any-hit shader is a separate invocation and the payload is the '
            'only shared state' % ', '.join(sorted(outside)))
    for b, i in fn.instrs():
        if b.label in body:
            continue
        for u in i.uses():
            if u in defined:
                raise rq.Unsupported(
                    'value %s defined in the Proceed loop is used after it; '
                    'the any-hit shader cannot return it' % u)


def _edit_committed(q, edits):
    """Committed accessors become payload reads."""
    for block, instr in q.committed_ops:
        idx, ty, align = PAYLOAD_FIELD[instr.dxop]
        if instr.dxop == rq.COMMITTED_BARY:
            comp = re.match(r'i8\s+(\d+)', instr.args[2].strip()).group(1)
            tmp = '%%rq.bv%s' % instr.result[1:]
            edits[instr.index] = (
                '  %s = load <2 x float>, <2 x float>* %%rq.pl1, align 4\n'
                '  %s = extractelement <2 x float> %s, i32 %s'
                % (tmp, instr.result, tmp, comp))
        else:
            edits[instr.index] = '  %s = load %s, %s* %%rq.pl%d, align %d' % (
                instr.result, ty, ty, idx, align)


# --- generated text --------------------------------------------------------

def _add_types_and_globals(text, globals_):
    decls = ['%s = type %s' % (PAYLOAD, PAYLOAD_TYPE),
             '%s = type { <2 x float> }' % ATTRS, '']
    seen = set()
    for sym, gty, elem, _ in globals_.values():
        if sym in seen:
            continue
        seen.add(sym)
        decls.append('@%s = external constant %s, align 4' % (sym, gty))
    anchor = re.search(r'^%dx\.types\.Handle = type .*$', text, re.M)
    if not anchor:
        raise LowerError('cannot find %dx.types.Handle to anchor declarations')
    return text[:anchor.end()] + '\n' + '\n'.join(decls) + text[anchor.end():]


def _swap_declarations(text, q, globals_):
    """Drop the rayQuery and compute-only declares, add the library ones."""
    for pat in [r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.rayQuery_[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.allocateRayQuery[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandle\(i32, i8[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.threadId[^\n]*\n']:
        text = re.sub(pat, '\n', text)

    new = ['', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #0',
           '', '; Function Attrs: nounwind',
           'declare void @dx.op.traceRay.%s(i32, %%dx.types.Handle, i32, i32, i32, '
           'i32, i32, float, float, float, float, float, float, float, float, %s*) #1'
           % (PAYLOAD[1:], PAYLOAD),
           '', '; Function Attrs: nounwind readonly',
           'declare float @dx.op.rayTCurrent.f32(i32) #2',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.instanceIndex.i32(i32) #0',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.primitiveIndex.i32(i32) #0']
    if q.loop:
        new += ['', '; Function Attrs: noreturn nounwind',
                'declare void @dx.op.ignoreHit(i32) #3']
    seen = set()
    for sym, gty, elem, _ in globals_.values():
        fnname, _ = _handle_fn(elem)
        if fnname in seen:
            continue
        seen.add(fnname)
        new += ['', '; Function Attrs: nounwind readonly',
                'declare %%dx.types.Handle @%s(i32, %s) #2' % (fnname, elem)]

    anchor = text.index('\nattributes #0 =')
    text = text[:anchor] + '\n' + '\n'.join(new) + text[anchor:]
    if q.loop and 'attributes #3' not in text:
        text = text.replace('attributes #2 = { nounwind readonly }',
                            'attributes #2 = { nounwind readonly }\n'
                            'attributes #3 = { noreturn nounwind }')
    return text


def _append_shaders(text, module, q, exports, table, globals_):
    """Emit the generated shader set after the raygen."""
    fns = []
    if q.loop:
        fns.append(_anyhit(module, q, exports, table, globals_))
    ch = ['define void @{ch}({pl}* noalias nocapture %p, {at}* nocapture readonly %attr) #1 {{'
          .format(ch=exports['closesthit'], pl=PAYLOAD, at=ATTRS),
          '  %t = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()',
          '  %ap = getelementptr inbounds {at}, {at}* %attr, i32 0, i32 0'.format(at=ATTRS),
          '  %b = load <2 x float>, <2 x float>* %ap, align 4',
          '  %pt = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 0'.format(pl=PAYLOAD),
          '  store float %t, float* %pt, align 4',
          '  %pb = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 1'.format(pl=PAYLOAD),
          '  store <2 x float> %b, <2 x float>* %pb, align 4',
          '  %ph = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 2'.format(pl=PAYLOAD),
          '  store i32 1, i32* %ph, align 4']
    # The index fields. Emitted unconditionally: unused ones cost a dead call
    # the driver removes, and making them conditional is another place to get
    # the payload layout wrong.
    for idx, (ty, call) in sorted(CH_SOURCE.items()):
        ch.append('  %%id%d = %s' % (idx, call))
        ch.append('  %%pi%d = getelementptr inbounds %s, %s* %%p, i32 0, i32 %d'
                  % (idx, PAYLOAD, PAYLOAD, idx))
        ch.append('  store %s %%id%d, %s* %%pi%d, align 4' % (ty, idx, ty, idx))
    ch += ['  ret void', '}']
    fns.append('\n'.join(ch))
    fns.append('''define void @{ms}({pl}* noalias nocapture %p) #1 {{
  %ph = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 2
  store i32 0, i32* %ph, align 4
  ret void
}}'''.format(ms=exports['miss'], pl=PAYLOAD))

    marker = re.search(r'^\}\s*$', text, re.M)
    if not marker:
        raise LowerError('cannot find the end of the entry function')
    return text[:marker.end()] + '\n\n' + '\n\n'.join(fns) + text[marker.end():]


def _anyhit(module, q, exports, table, globals_):
    """The Proceed loop body, re-rooted as an any-hit shader.

    Accept is falling off the end; reject is IgnoreHit. Note the polarity is
    the reverse of the RayQuery form, where committing is the special path."""
    fn = q.fn
    header, latch, body = q.loop
    commit_blocks = {b.label for b, _ in q.commits}

    subst = {}
    for label in body:
        if label == latch:
            continue
        for i in fn.block(label).instrs:
            if i.dxop == rq.CANDIDATE_TYPE:
                # An any-hit shader runs only for non-opaque triangle
                # candidates, so this test is a tautology here.
                subst[i.result] = '0'

    order = [header] + sorted(l for l in body if l not in (header, latch))
    out = ['define void @%s(%s* noalias nocapture %%p, %s* nocapture readonly %%attr) #1 {'
           % (exports['anyhit'], PAYLOAD, ATTRS),
           '  %rq.ap = getelementptr inbounds {at}, {at}* %attr, i32 0, i32 0'.format(at=ATTRS),
           '  %rq.ab = load <2 x float>, <2 x float>* %rq.ap, align 4']

    # Recreate every resource handle the body uses, under the SAME SSA name it
    # had in the raygen, so the transplanted instructions need no rewriting.
    used = set()
    for label in body:
        if label != latch:
            for i in fn.block(label).instrs:
                used.update(i.uses())
    for _, i in fn.instrs():
        if i.dxop == 57 and i.result in used:
            out.append(_handle_text(i, table, globals_))

    for label in order:
        blk = fn.block(label)
        if label != header:
            out.append('')
            out.append('%s:' % label[1:])
        for i in blk.instrs:
            if i.dxop == rq.CANDIDATE_TYPE or i.dxop == rq.COMMIT_NON_OPAQUE:
                continue
            if i.dxop == rq.CANDIDATE_BARY:
                comp = re.match(r'i8\s+(\d+)', i.args[2].strip()).group(1)
                out.append('  %s = extractelement <2 x float> %%rq.ab, i32 %s'
                           % (i.result, comp))
                continue
            line = i.line
            for old, new in subst.items():
                line = re.sub(re.escape(old) + r'\b', new, line)
            # An edge to the latch leaves the loop body: through a commit it
            # means the candidate was accepted, otherwise rejected.
            target = '%rq.accept' if label in commit_blocks else '%rq.reject'
            line = re.sub(r'label\s+' + re.escape(latch) + r'\b', 'label ' + target, line)
            for s in Block_successors(line):
                if s not in body and s not in ('%rq.accept', '%rq.reject'):
                    raise rq.Unsupported(
                        'Proceed loop body branches to %s, outside the loop; '
                        'no lowering is defined for that' % s)
            out.append(line)

    out += ['', 'rq.accept:', '  ret void',
            '', 'rq.reject:',
            '  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()',
            '  unreachable', '}']
    return '\n'.join(out)


def Block_successors(line):
    return re.findall(r'label\s+(%[\w.$-]+)', line)


def _rewrite_metadata(text, md, table, globals_, q, exports):
    """Point the resource records at the globals and rebuild the entry points."""
    for (cls, nid), (sym, gty, elem, name) in globals_.items():
        fields = [f.strip() for f in md[nid].split(',')]
        fields[1] = re.sub(r'\*\s+\S+$', '* @%s' % sym, fields[1])
        fields[2] = '!"%s"' % name
        text = re.sub(r'^!%d = !\{.*\}\s*$' % nid,
                      '!%d = !{%s}' % (nid, ', '.join(fields)), text, flags=re.M)

    text = re.sub(r'^!(\d+) = !\{!"cs", i32 (\d+), i32 (\d+)\}\s*$',
                  lambda m: '!%s = !{!"lib", i32 %s, i32 %s}'
                            % (m.group(1), m.group(2), m.group(3)),
                  text, flags=re.M)

    m = re.search(r'!dx\.entryPoints\s*=\s*!\{!(\d+)\}', text)
    old_entry = int(m.group(1))
    res = re.search(r'!dx\.resources\s*=\s*!\{!(\d+)\}', text).group(1)

    # Fresh metadata ids above everything the module already uses.
    counter = [max(md) + 1]

    def node(body):
        """Emit a metadata node and return its !id."""
        nid = counter[0]
        counter[0] += 1
        nodes.append('!%d = !{%s}' % (nid, body))
        return nid

    nodes = []
    sig_h = '(%s*, %s*)' % (PAYLOAD, ATTRS)
    sig_p = '(%s*)' % PAYLOAD

    # Type annotations. Shapes copied from a DXC-built library: a return
    # annotation plus one node per parameter, tag 2 for the payload and tag 0
    # for the attributes.
    empty = node('')
    ret = node('i32 1, !%d, !%d' % (empty, empty))
    par_payload = node('i32 2, !%d, !%d' % (empty, empty))
    par_attrs = node('i32 0, !%d, !%d' % (empty, empty))
    ann_raygen = node('!%d' % ret)
    ann_hit = node('!%d, !%d, !%d' % (ret, par_payload, par_attrs))
    ann_miss = node('!%d, !%d' % (ret, par_payload))

    ann = ['i32 1', 'void ()* @%s' % exports['raygen'], '!%d' % ann_raygen]
    if q.loop:
        ann += ['void %s* @%s' % (sig_h, exports['anyhit']), '!%d' % ann_hit]
    ann += ['void %s* @%s' % (sig_h, exports['closesthit']), '!%d' % ann_hit]
    ann += ['void %s* @%s' % (sig_p, exports['miss']), '!%d' % ann_miss]
    ann_id = node(', '.join(ann))

    # Entry points: a resource-only record, then one per export. Tags are
    # 8 shader kind, 6 payload bytes, 7 attribute bytes, 5 auto binding space.
    zero = node('i32 0')
    flags = node('i32 0, i64 16')
    eps = [node('null, !"", null, !%s, !%d' % (res, flags))]

    def entry(name, sig, kind, payload, attrs):
        # Which property tags appear depends on the shader kind, exactly as
        # DXC emits them: a raygen carries neither size, a miss carries the
        # payload size only, and hit shaders carry both.
        props = ['i32 8', 'i32 %d' % kind]
        if payload:
            props += ['i32 6', 'i32 %d' % PAYLOAD_BYTES]
        if attrs:
            props += ['i32 7', 'i32 8']
        props += ['i32 5', '!%d' % zero]
        pid = node(', '.join(props))
        return node('void %s* @%s, !"%s", null, null, !%d'
                    % (sig, name, name, pid))

    if q.loop:
        eps.append(entry(exports['anyhit'], sig_h, 9, True, True))
    eps.append(entry(exports['closesthit'], sig_h, 10, True, True))
    eps.append(entry(exports['miss'], sig_p, 11, True, False))
    eps.append(entry(exports['raygen'], '()', 7, False, False))

    text = text.replace('!dx.entryPoints = !{!%d}' % old_entry,
                        '!dx.typeAnnotations = !{!%d}\n!dx.entryPoints = !{%s}'
                        % (ann_id, ', '.join('!%d' % e for e in eps)))
    # The old compute entry record and its numthreads node are dead.
    text = re.sub(r'^!%d = !\{void \(\)\* @\w+.*\}\s*$' % old_entry, '', text, flags=re.M)
    return text.rstrip('\n') + '\n' + '\n'.join(nodes) + '\n'
