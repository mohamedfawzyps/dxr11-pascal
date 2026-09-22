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
#   0 t | 1 bary | 2 hit | 3 inst | 4 prim | 5 unused | 6 instanceID
#   7 hitKind | 8 worldToObject, 12 floats | 9 aborted
#
# The LAYOUT is fixed even though the matrix is only sometimes read: variable
# offsets across two implementations is a good way to get one of them subtly
# wrong. The per-invocation COST is what is made conditional instead, since the
# closest-hit only fetches the matrix when the shader actually reads it.
PAYLOAD_TYPE = ('{ float, <2 x float>, i32, i32, i32, i32, i32, i32, '
                '[12 x float], i32, i32 }')
PAYLOAD_BYTES = 92
PAYLOAD_FIELD = {
    rq.COMMITTED_RAY_T: (0, 'float', 8),
    rq.COMMITTED_BARY: (1, '<2 x float>', 4),
    rq.COMMITTED_STATUS: (2, 'i32', 4),
    rq.COMMITTED_INSTANCE_INDEX: (3, 'i32', 4),
    rq.COMMITTED_PRIMITIVE_INDEX: (4, 'i32', 4),
    rq.COMMITTED_GEOMETRY_INDEX: (5, 'i32', 4),
    rq.COMMITTED_INSTANCE_ID: (6, 'i32', 4),
    rq.COMMITTED_FRONT_FACE: (7, 'i32', 4),
    rq.COMMITTED_WORLD_TO_OBJECT: (8, '[12 x float]', 4),
    # 9 is the abort flag. 10 was added with the record constants; the geometry
    # index needed no new field because slot 5 had been reserved for it all
    # along and left unused while the accessor was refused.
    rq.COMMITTED_INSTANCE_CONTRIB: (10, 'i32', 4),
}

# The record's own two numbers, and how the generated shaders reach them.
#
# Not a dx.op call like everything else in CH_SOURCE: there is no intrinsic for
# either, which is exactly why they were refused. They come out of a cbuffer
# bound by a LOCAL root signature, so the value is per hit-group-record and the
# shim chose it when it built the table.
RECORD_TYPE = '%rq_record'
RECORD_GLOBAL = '@rq_record'
RECORD_HANDLE_FN = 'dx.op.createHandleForLib.rq_record'
CBRET = '%dx.types.CBufRet.i32'
# Which word of the record each accessor reads.
RECORD_WORD = {
    rq.CANDIDATE_GEOMETRY_INDEX: 0,
    rq.COMMITTED_GEOMETRY_INDEX: 0,
    rq.CANDIDATE_INSTANCE_CONTRIB: 1,
    rq.COMMITTED_INSTANCE_CONTRIB: 1,
}


def _record_read(tag, word, result):
    """The four lines that pull one word out of the hit group record.

    Emitted per use rather than hoisted. The handle and the load are pure and
    the driver folds the duplicates; hoisting them by hand would mean finding a
    place to put them that dominates every use, which is a CFG question this
    pass has no reason to ask."""
    return [
        '  %%rq.cbv%s = load %s, %s* %s, align 4'
        % (tag, RECORD_TYPE, RECORD_TYPE, RECORD_GLOBAL),
        '  %%rq.cbh%s = call %%dx.types.Handle @%s(i32 160, %s %%rq.cbv%s)'
        '  ; CreateHandleForLib(Resource)' % (tag, RECORD_HANDLE_FN, RECORD_TYPE, tag),
        '  %%rq.cbr%s = call %s @dx.op.cbufferLoadLegacy.i32(i32 59, '
        '%%dx.types.Handle %%rq.cbh%s, i32 0)  ; CBufferLoadLegacy(handle,regIndex)'
        % (tag, CBRET, tag),
        '  %s = extractvalue %s %%rq.cbr%s, %d' % (result, CBRET, tag, word),
    ]

# RayQuery -> DXR 1.0, for accessors read in the ANY-HIT shader. Measured from
# DXC output on both sides. The operand shape is identical apart from the query
# handle, which simply goes away.
CANDIDATE_MAP = {
    rq.CANDIDATE_INSTANCE_INDEX:  ('i32', 'dx.op.instanceIndex.i32', 142, 0),
    rq.CANDIDATE_INSTANCE_ID:     ('i32', 'dx.op.instanceID.i32', 141, 0),
    rq.CANDIDATE_PRIMITIVE_INDEX: ('i32', 'dx.op.primitiveIndex.i32', 161, 0),
    rq.CANDIDATE_RAY_T:           ('float', 'dx.op.rayTCurrent.f32', 154, 0),
    rq.CANDIDATE_OBJECT_RAY_ORIGIN:    ('float', 'dx.op.objectRayOrigin.f32', 149, 1),
    rq.CANDIDATE_OBJECT_RAY_DIRECTION: ('float', 'dx.op.objectRayDirection.f32', 150, 1),
    rq.CANDIDATE_WORLD_TO_OBJECT:      ('float', 'dx.op.worldToObject.f32', 152, 2),
    rq.RAY_FLAGS:                      ('i32', 'dx.op.rayFlags.i32', 144, 0),
}
# HitKind() is an integer; the RayQuery form is a bool. 254 is
# HIT_KIND_TRIANGLE_FRONT_FACE.
HIT_KIND_FRONT = 254
# Which dx.op gives each field in a DXR 1.0 closest-hit, read off DXC output.
CH_SOURCE = {
    3: ('i32', 'call i32 @dx.op.instanceIndex.i32(i32 142)'),
    4: ('i32', 'call i32 @dx.op.primitiveIndex.i32(i32 161)'),
    6: ('i32', 'call i32 @dx.op.instanceID.i32(i32 141)'),
    7: ('i32', 'call i32 @dx.op.hitKind.i32(i32 143)'),
    # Field 5, geometry index, is deliberately absent: see NO_LOWERING in
    # rayquery.py. Emitting dx.op.geometryIndex would set shader flag
    # 0x2000000 and the state object would then be refused by the driver.
}


class LowerError(Exception):
    pass


# --- resources -------------------------------------------------------------

# `distinct` counts. A [branch] or [loop] hint makes DXC emit
# `!16 = distinct !{!16, !"dx.controlflow.hints", i32 1}`, and a pattern
# that misses it leaves that id invisible. The fresh-id counter starts at
# max(md)+1, so it then hands out an id the module already uses and the
# assembler says "Metadata id is already used". Unreal uses those hints
# constantly.
_MD = re.compile(r'^!(\d+) = (?:distinct )?!\{(.*)\}\s*$', re.M)
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
                          'closesthit': 'ClosestHit', 'miss': 'Miss',
                          'intersection': 'Isect',
                          # Only a both-kinds query uses this: a procedural
                          # hit and a triangle hit report DIFFERENT committed
                          # statuses, 2 and 1, and one closest-hit cannot say
                          # both. Everything else keeps a single ClosestHit, so
                          # no other output moves by a byte.
                          'closesthitproc': 'ClosestHitProc',
                          # Hit groups that must never commit. A scene can
                          # route triangle and procedural geometry to
                          # DIFFERENT records, and the shim has a real hit
                          # group for only one of those kinds; the other index
                          # gets a record of the right TYPE that finds nothing.
                          # Emitted always, because which one a scene needs is
                          # not knowable when the shader is lowered: the
                          # acceleration structures do not exist yet. Two tiny
                          # functions is a cheap price for not having to
                          # re-lower later.
                          'anyhitnull': 'AnyHitNull',
                          'isectnull': 'IsectNull'}
    text = module.text
    md = _metadata(text)
    table = _resource_table(text, md)
    edits = {}

    binding = _uses_binding(module)
    binds = _binding_map(text, md) if binding else None
    globals_ = _plan_globals(table)
    _edit_resources(module, q, edits, table, globals_, binds)
    _edit_ray_index(q, edits)
    _edit_query(module, q, edits, exports)

    fn = q.fn
    edits[fn.index] = 'define void @%s() #1 {' % exports['raygen']

    out = render(module, edits)
    out = _add_types_and_globals(out, globals_, q.needs_record_constants, binding)
    out = _swap_declarations(out, q, globals_, binding)
    out = _append_shaders(out, module, q, exports, table, globals_, binds)
    out = _rewrite_metadata(out, md, table, globals_, q, exports, binding)
    return out


# Shader Model 6.6 reaches a resource by BINDING, not by a range index.
#
# Unreal's RayQuery shaders are cs_6_6 and every one of them does this. The
# rewriter was built against 6.5, and that single difference caused 124 of the
# 157 refusals a real Unreal run produced: handles made this way were not on
# the recomputable list, and the globals synthesised for them were never loaded.
#
#   cs_6_6   createHandleFromBinding (217) -> annotateHandle (216)
#   lib_6_6  createHandleForLib      (160) -> annotateHandle (216)
#
# So the conversion is 217 -> 160, and the annotateHandle after it is kept
# untouched. Read off DXC, see phase5/cases/reference/lib_sm66_binding_ref.hlsl.
BIND_HANDLE = 217
ANNOTATE_HANDLE = 216
# The library form's global is a HANDLE, not the resource type, and the
# overload is named after the handle type too. That is the part that cannot be
# guessed from the 6.5 path, where both are the resource type.
HANDLE_TYPE = '%dx.types.Handle'


# The module-level shader flags, tag 0 of the entry point's properties.
#
# The lowering used to hardcode 16 here, which happened to be right for every
# shader this project had ever seen: they all declared 0x2000010, and the only
# bit the lowering removes is 0x2000000. An Unreal shader declares 0x42000010,
# so 16 was wrong by exactly the bit it did not know about and the validator
# said "Flags must match usage".
#
# Carrying the value through and clearing what the lowering removes is right in
# both cases, and reproduces the old output byte for byte.
#
# 0x2000000 is the raytracing tier 1.1 shader flag: measured on GeometryIndex,
# which sets it and makes CreateStateObject refuse the library on a GTX 1070.
# Every RayQuery op is gone after lowering, so the flag must go with them.
RT11_SHADER_FLAG = 0x2000000


def _module_flags(text, md):
    m = re.search(r'!dx\.entryPoints\s*=\s*!\{!(\d+)\}', text)
    if not m:
        return 0
    fields = [f.strip() for f in md[int(m.group(1))].split(',')]
    if len(fields) < 5 or not fields[4].startswith('!'):
        return 0
    props = md.get(int(fields[4][1:]), '')
    p = [x.strip() for x in props.split(',')]
    for i in range(0, len(p) - 1, 2):
        if p[i] == 'i32 0':
            v = re.match(r'i64\s+(\d+)', p[i + 1])
            if v:
                return int(v.group(1))
    return 0


def _uses_binding(module):
    return any(i.dxop == BIND_HANDLE for _, i in module.functions[0].instrs()) \
        if module.functions else False


def _resbind(instr):
    """(class, space, lowerBound) from a createHandleFromBinding operand.

    `%dx.types.ResBind { i32 lower, i32 upper, i32 space, i8 class }`, read off
    real DXC output: `{ i32 4, i32 4, i32 0, i8 2 }` is cbuffer b4, space 0.
    """
    # SRV t0 space0 is all zeroes, and LLVM prints an all-zero struct as
    # `zeroinitializer` rather than writing the fields out. Nothing in the
    # Unreal shaders was bound there, so only an independently written case
    # reached this.
    if 'zeroinitializer' in instr.args[1]:
        return 0, 0, 0
    m = re.search(r'\{\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),'
                  r'\s*i8\s+(-?\d+)\s*\}', instr.args[1])
    if not m:
        raise LowerError('cannot read the ResBind of %s' % instr.body[:60])
    return int(m.group(4)), int(m.group(3)), int(m.group(1))


def _binding_map(text, md):
    """{(class, space, lowerBound) -> record node id}.

    A record is `{id, global, name, space, lowerBound, rangeSize, ...}`, so the
    binding in the call is matched to the record by space and lower bound
    rather than by the range index the 6.5 form carries."""
    m = re.search(r'!dx\.resources\s*=\s*!\{!(\d+)\}', text)
    if not m:
        raise LowerError('module has no !dx.resources')
    out = {}
    for cls, g in enumerate(x.strip() for x in md[int(m.group(1))].split(',')):
        if g == 'null':
            continue
        for tok in md[int(g[1:])].split(','):
            nid = int(tok.strip()[1:])
            f = [x.strip() for x in md[nid].split(',')]
            sp = re.match(r'i32\s+(-?\d+)', f[3])
            lo = re.match(r'i32\s+(-?\d+)', f[4])
            if sp and lo:
                out[(cls, int(sp.group(1)), int(lo.group(1)))] = nid
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


def _handle_text(instr, table, globals_, binds=None):
    """The load + createHandleForLib that replaces one handle creation.

    Shared, because the any-hit shader has to recreate any resource handle the
    Proceed loop body used. A handle is not caller state: it names a resource,
    and every shader in the library can reach it."""
    if instr.dxop == BIND_HANDLE:
        # Shader Model 6.6. The binding names the resource directly, so the
        # record is found by space and lower bound rather than by a range
        # index. The annotateHandle that follows is left exactly as it was: it
        # is legal in a library and carries the resource properties.
        cls, space, lower = _resbind(instr)
        nid = (binds or {}).get((cls, space, lower))
        if nid is None:
            raise LowerError(
                'createHandleFromBinding names class %d space %d register %d, '
                'which !dx.resources does not describe' % (cls, space, lower))
        sym, _, _, _ = globals_[(cls, nid)]
        idx = re.match(r'i32\s+(\d+)$', instr.args[2].strip())
        if not idx:
            # The 6.5 path DOES support this, through a getelementptr on the
            # array global. The 6.6 form is refused only because the shape of
            # an ARRAY global in the binding form has not been measured off
            # DXC, and guessing it is how a lowering goes silently wrong.
            # phase5/cases/reference/ is where that question gets answered.
            raise rq.Unsupported(
                'resource handle %s comes from an array binding indexed '
                'dynamically (%s); the Shader Model 6.6 form of that has not '
                'been measured, and the 6.5 form is what this lowers'
                % (instr.result, instr.args[2].strip()))
        raw = '%%rq.raw.%s' % instr.result[1:]
        return (
            '  {raw} = load {h}, {h}* @{sym}, align 4\n'
            '  {res} = call {h} @dx.op.createHandleForLib.dx.types.Handle'
            '(i32 160, {h} {raw})  ; CreateHandleForLib(Resource)'
        ).format(raw=raw, h=HANDLE_TYPE, sym=sym, res=instr.result)

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

    pre = ''
    if gty == elem:
        src = '{ty}* @{sym}'.format(ty=elem, sym=sym)
    else:
        # A resource array. DXC reaches the element through a getelementptr on
        # the array global and hands createHandleForLib the ELEMENT type.
        # Matched against a DXC-built library, see
        # phase5/cases/reference/lib_array_ref.hlsl for the constant form and
        # lib_dynarray_ref.hlsl for the dynamic one.
        idx = re.match(r'i32\s+(\d+)$', instr.args[3].strip())
        nonuni = instr.args[4].strip() not in ('i1 false', 'i1 0')
        if idx:
            # A constant index folds into a constant getelementptr EXPRESSION,
            # inline in the call. Nothing is computed at runtime.
            src = ('{elem}* getelementptr inbounds ({gty}, {gty}* @{sym}, '
                   'i32 0, i32 {i})').format(elem=elem, gty=gty, sym=sym,
                                             i=idx.group(1))
        else:
            # A dynamic index cannot be a constant expression, so it becomes a
            # real getelementptr INSTRUCTION on the same global, with the
            # index operand carried across untouched. Same three steps DXC
            # emits: getelementptr, load, createHandleForLib.
            gep = '%%rq.gep.%s' % instr.result[1:]
            # NonUniformResourceIndex shows up as !dx.nonuniform on the
            # getelementptr, and dropping it would be a silently wrong lowering
            # rather than a missing feature. The node id is not known until the
            # metadata is rebuilt, so a placeholder stands in until then.
            md = '  ; index is dynamic'
            tag = ', !dx.nonuniform !RQNU' if nonuni else ''
            pre = ('  {gep} = getelementptr inbounds {gty}, {gty}* @{sym}, '
                   'i32 0, {idx}{tag}\n').format(
                       gep=gep, gty=gty, sym=sym,
                       idx=instr.args[3].strip(), tag=tag)
            src = '{elem}* {gep}'.format(elem=elem, gep=gep)

    return pre + (
        '  {raw} = load {elem}, {src}, align 4\n'
        '  {res} = call %dx.types.Handle @{fn}(i32 160, {elem} {raw})'
        '  ; CreateHandleForLib(Resource)'
    ).format(raw=raw, elem=elem, src=src, res=instr.result, fn=fnname)


def _dynamic_index(instr):
    """The index operand of a createHandle, when it is NOT a constant."""
    if instr.dxop != 57 or len(instr.args) < 4:
        return None
    arg = instr.args[3].strip()
    return None if re.match(r'i32\s+\d+$', arg) else arg


def _edit_resources(module, q, edits, table, globals_, binds=None):
    """Handle creation -> load + createHandleForLib, in either binding form."""
    for block, instr in q.fn.instrs():
        if instr.dxop not in (57, BIND_HANDLE):
            continue
        edits[instr.index] = _handle_text(instr, table, globals_, binds)


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
        _check_loop_isolated(fn, q, header, latch, body, _type_names(module.text))

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
    # After the loop blocks have been marked for deletion, so a RayFlags
    # read INSIDE the loop keeps its deletion and only the raygen ones get
    # the constant.
    _edit_ray_flags(q, edits)


def _trace_block(q, ra, exit_label):
    # RayContributionToHitGroupIndex stays 0 and MissShaderIndex stays 0, so
    # the record a hit lands on is the instance contribution plus the geometry
    # multiplier times the geometry index.
    #
    # The multiplier is 1 only when the shader asks what geometry it hit. At 0
    # every geometry of an instance shares one record, which is what this did
    # before and is one record instead of one per geometry; turning it on
    # unconditionally would grow every scene's table for nothing.
    """Payload init plus the TraceRay that replaces the inline query."""
    geom_mult = 1 if q.needs_record_constants else 0
    flag_setup, flag_text = q.flags_operand
    lines = []
    for idx, ty, align in [(0, 'float', 8), (1, '<2 x float>', 4), (2, 'i32', 4),
                           (3, 'i32', 4), (4, 'i32', 4), (5, 'i32', 4),
                           (6, 'i32', 4), (7, 'i32', 4), (8, '[12 x float]', 4),
                           (9, 'i32', 4), (10, 'i32', 4)]:
        zero = '0.000000e+00' if ty == 'float' else (
            'zeroinitializer' if (ty.startswith('<') or ty.startswith('[')) else '0')
        lines.append('  %%rq.pl%d = getelementptr inbounds %s, %s* %%rq.pl, i32 0, i32 %d'
                     % (idx, PAYLOAD, PAYLOAD, idx))
        lines.append('  store %s %s, %s* %%rq.pl%d, align %d' % (ty, zero, ty, idx, align))
    lines += flag_setup
    lines += [
        '  call void @dx.op.traceRay.%s(i32 157, %%dx.types.Handle %s, %s, '
        '%s, i32 0, i32 %d, i32 0, %s, %s, %s, %s, %s, %s, %s, %s, %s* nonnull %%rq.pl)'
        '  ; TraceRay(...)' % (
            PAYLOAD[1:], q.as_handle, flag_text, ra['mask'], geom_mult,
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


# Instructions the any-hit shader can simply RUN AGAIN, because their answer
# does not depend on anything only the raygen knows.
#
# This is the same reasoning that exempted resource handles, one level up. A
# handle is not caller state because it names a resource every shader in the
# library can reach; a shader parameter loaded from a cbuffer is not caller
# state either, because the cbuffer is bound by the GLOBAL root signature and
# holds the same bytes for every invocation of the dispatch. Neither is a value
# computed from those by pure arithmetic.
#
# That matters because it is the ordinary shape of an alpha test: read the
# thresholds before the loop, compare against them inside it. Unreal refused 59
# shaders here.
#
# The list is deliberately short, and what is NOT on it is the point:
#
#   load / any buffer or texture fetch
#       a UAV the raygen wrote before the loop would read back differently.
#       A read inside the loop is a different thing and is already supported.
#   phi
#       its value depends on which path the RAYGEN took to reach it, which is
#       exactly the caller state the payload cannot carry.
#   sdiv / udiv / srem / urem
#       recomputing hoists the operation, and integer division by zero is
#       undefined. A pure operation that cannot trap is safe to hoist; one that
#       can is not.
#   any other dx.op
#       not enumerated means not verified, the same rule the opcode whitelist
#       follows.
_PURE = {
    'add', 'sub', 'mul', 'and', 'or', 'xor', 'shl', 'lshr', 'ashr',
    'fadd', 'fsub', 'fmul', 'fdiv', 'fneg',
    'icmp', 'fcmp', 'select', 'extractvalue', 'extractelement',
    'zext', 'sext', 'trunc', 'bitcast', 'sitofp', 'uitofp', 'fptosi',
    'fptoui', 'fpext', 'fptrunc', 'getelementptr',
}
# dx.op calls that are recomputable. 57 is createHandle and 217 is its Shader
# Model 6.6 replacement, both exempt and both re-emitted through _handle_text.
# 216 is annotateHandle, which follows 217 and carries only constant resource
# properties. 59 is cbufferLoadLegacy. 93 is threadId, which the any-hit reaches
# as DispatchRaysIndex: the SAME ray, so the same value, and legal in every ray
# tracing stage.
#
# 216 and 217 being absent is what refused 118 of Unreal's shaders: every one of
# them reaches its cbuffer through the 6.6 binding form, so no handle was ever
# exempt and every value built on one looked like caller state.
_PURE_DXOP = {57, 59, 93, ANNOTATE_HANDLE, BIND_HANDLE}


# Every %name an instruction READS, not just its call arguments.
#
# Instr.uses() decodes call arguments and phi incomings, which is what the rest
# of this pass needs. It is not enough for the isolation check: a value can
# reach the loop body through ordinary arithmetic, `fcmp float %bary, %thresh`,
# and uses() cannot see it. The check then let it through, the generated hit
# shader referenced a value it never defined, and the ASSEMBLER reported it, as
# "use of undefined value". Loud, but nowhere near the cause.
#
# Types are told from values by asking the module, not by guessing: a name is a
# type exactly when the module declares `%name = type ...`. Guessing on the
# shape of the name does not work, because %rq.pl and %dx.types.Handle look
# alike.
_TYPEDEF = re.compile(r'^(%"[^"]*"|%[\w.$-]+)\s*=\s*type\s', re.M)
# A quoted name first, because %"class.RWStructuredBuffer<Result>" holds
# characters an unquoted one cannot and would otherwise be cut at the quote.
_NAME = re.compile(r'%"[^"]*"|%[\w.$-]+')


def _type_names(text):
    return set(_TYPEDEF.findall(text))


def _operands(instr, types):
    """The values this instruction reads, whatever kind of instruction it is."""
    # A phi writes its predecessors as `[ %val, %bb12 ]`, with no `label`
    # keyword to mark them, so the general scan below counts %bb12 as a value
    # read. Unreal's loop bodies are full of phis, and the check then refused
    # 118 shaders for "reading" their own predecessor blocks.
    #
    # uses() already decodes a phi correctly: incoming VALUES, which do count,
    # and not the blocks, which do not.
    if instr.is_phi:
        return instr.uses()
    body = instr.body
    cut = body.find(';')
    if cut >= 0:
        body = body[:cut]
    out = []
    for m in _NAME.finditer(body):
        n = m.group(0)
        if n in types or n == instr.result:
            continue
        # `label %bb42` is a branch target, not a value.
        before = body[:m.start()].rstrip()
        if before.endswith('label'):
            continue
        out.append(n)
    return out


def _recomputable(fn, types):
    """{result -> instr} for every value the generated hit shader can rebuild.

    A fixpoint, because a chain is only recomputable if every link is."""
    ok = {}
    changed = True
    while changed:
        changed = False
        for _, i in fn.instrs():
            if not i.result or i.result in ok:
                continue
            if i.dxop is not None and i.dxop not in _PURE_DXOP:
                continue
            if i.dxop is None:
                op = re.match(r'\s*%\S+\s*=\s*(\w+)', i.line)
                if not op or op.group(1) not in _PURE:
                    continue
            if all(u in ok or u == i.result for u in _operands(i, types)):
                ok[i.result] = i
                changed = True
    return ok


def _needed_chain(fn, recomputable, wanted, types, skip=()):
    """The recomputable instructions behind `wanted`, in source order.

    Source order is enough: every one of them is outside the loop and pure, so
    the raygen's order already respects their dependencies.

    `skip` is what the loop body defines for itself. Without it the closure
    walks straight back into the body and rebuilds its instructions in the
    prologue as well, which is a duplicate definition and, where the prologue
    copy lands first, a use of a value the body has not defined yet."""
    need = set()
    frontier = [w for w in wanted if w not in skip]
    while frontier:
        r = frontier.pop()
        if r in need or r in skip or r not in recomputable:
            continue
        need.add(r)
        frontier.extend(_operands(recomputable[r], types))
    return [i for _, i in fn.instrs() if i.result in need]


def _check_loop_isolated(fn, q, header, latch, body, types):
    """The any-hit shader is a separate invocation with only the payload for
    shared state, so the loop body must not read caller locals or leak values
    back out. Both are in the brief's no-valid-lowering table."""
    defined, used = set(), set()
    for label in body:
        blk = fn.block(label)
        for i in blk.instrs:
            if i.result:
                defined.add(i.result)
            used.update(_operands(i, types))

    # A resource handle read inside the loop is NOT caller state. It names a
    # resource, and the any-hit shader can create its own handle for the same
    # one, so those are recreated rather than refused. Real alpha testing reads
    # a texture or buffer in the loop body, so refusing this would have blocked
    # the most common shape there is.
    handles = set(_recomputable(fn, types))

    # But that exemption holds only for a handle the MODULE fully determines.
    # A dynamically indexed one depends on a value computed in the raygen, and
    # recreating it in the any-hit would need that value, which is exactly the
    # caller state the payload cannot carry. So this one IS refused, precisely,
    # rather than being swept up by the generic message below.
    for _, i in fn.instrs():
        if i.result in used and _dynamic_index(i):
            raise rq.Unsupported(
                'resource handle %s is indexed dynamically (%s) and used inside '
                'the Proceed loop; the index is computed in the raygen and the '
                'any-hit shader is a separate invocation that cannot see it'
                % (i.result, _dynamic_index(i)))
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
        for u in _operands(i, types):
            if u in defined:
                raise rq.Unsupported(
                    'value %s defined in the Proceed loop is used after it; '
                    'the any-hit shader cannot return it' % u)


def _edit_ray_flags(q, edits):
    """RayFlags() read in the raygen becomes the constant it must be.

    dx.op.rayFlags is legal in a hit or miss shader, not in a raygen, so the
    loop-body uses go through CANDIDATE_MAP and these do not. The value is the
    same one the lowering hands TraceRay, and the analysis refuses a query whose
    flags are not a compile-time constant, so there is nothing to look up at
    runtime."""
    for _, instr in q.candidate_ops:
        if instr.dxop != rq.RAY_FLAGS:
            continue
        # A read inside the loop already has an edit: the whole block is marked
        # for deletion, and the loop body is re-emitted into the any-hit where
        # dx.op.rayFlags IS legal. Overwriting that entry would resurrect the
        # instruction in the raygen as well as the any-hit.
        if instr.index in edits:
            q.rayflags_in_loop = True
            continue
        if q.dyn_flags is not None:
            edits[instr.index] = '  %s = add i32 %d, 0' % (instr.result, q.ray_flags)
        else:
            # RayFlags() can only be called after TraceRayInline, so the
            # runtime operand dominates this point.
            from dxil import operand_name
            edits[instr.index] = '  %s = or i32 %s, %d' % (
                instr.result, operand_name(q.trace.args[3]), q.const_flags or 0)


def _edit_committed(q, edits):
    """Committed accessors become payload reads."""
    for block, instr in q.committed_ops:
        idx, ty, align = PAYLOAD_FIELD[instr.dxop]
        if instr.dxop == rq.COMMITTED_FRONT_FACE:
            tmp = '%%rq.ff%s' % instr.result[1:]
            edits[instr.index] = (
                '  %s = load i32, i32* %%rq.pl7, align 4\n'
                '  %s = icmp eq i32 %s, %d'
                % (tmp, instr.result, tmp, HIT_KIND_FRONT))
        elif instr.dxop == rq.COMMITTED_WORLD_TO_OBJECT:
            row = re.match(r'i32\s+(\d+)', instr.args[2].strip()).group(1)
            col = re.match(r'i8\s+(\d+)', instr.args[3].strip()).group(1)
            slot = int(row) * 4 + int(col)
            ptr = '%%rq.w%s' % instr.result[1:]
            edits[instr.index] = (
                '  %s = getelementptr inbounds [12 x float], [12 x float]* '
                '%%rq.pl8, i32 0, i32 %d\n'
                '  %s = load float, float* %s, align 4'
                % (ptr, slot, instr.result, ptr))
        elif instr.dxop == rq.COMMITTED_BARY:
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

def _add_types_and_globals(text, globals_, needs_record=False, binding=False):
    decls = ['%s = type %s' % (PAYLOAD, PAYLOAD_TYPE),
             '%s = type { <2 x float> }' % ATTRS, '']
    if needs_record:
        # Two words, matching the two root constants the shim puts in every hit
        # group record. Declared here even when the original shader had no
        # cbuffer of its own, which is why CBufRet is added conditionally too.
        decls.insert(2, '%s = type { i32, i32 }' % RECORD_TYPE)
        if ('%s = type' % CBRET) not in text:
            decls.insert(3, '%s = type { i32, i32, i32, i32 }' % CBRET)
        decls.append('%s = external constant %s, align 4'
                     % (RECORD_GLOBAL, RECORD_TYPE))
    seen = set()
    for sym, gty, elem, _ in globals_.values():
        if sym in seen:
            continue
        seen.add(sym)
        # In the binding form the global holds a HANDLE whatever the resource
        # is; the RECORD bitcasts it back to the resource type. Copied from
        # DXC: `@CB = external constant %dx.types.Handle`.
        decls.append('@%s = external constant %s, align 4'
                     % (sym, HANDLE_TYPE if binding else gty))
    anchor = re.search(r'^%dx\.types\.Handle = type .*$', text, re.M)
    if not anchor:
        raise LowerError('cannot find %dx.types.Handle to anchor declarations')
    return text[:anchor.end()] + '\n' + '\n'.join(decls) + text[anchor.end():]


def _swap_declarations(text, q, globals_, binding=False):
    """Drop the rayQuery and compute-only declares, add the library ones."""
    for pat in [r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.rayQuery_[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.allocateRayQuery[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandle\(i32, i8[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.threadId[^\n]*\n',
                # The 6.6 handle creation is converted away, so its declare
                # goes too: an unused declare is itself a validation error.
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandleFromBinding[^\n]*\n']:
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
           'declare i32 @dx.op.primitiveIndex.i32(i32) #0',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.instanceID.i32(i32) #0',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.hitKind.i32(i32) #0',
           ]
    # An UNUSED declare is itself a validation error, so these three are
    # emitted only when something actually calls them. The rest above are
    # always used, because the generated closest-hit reads them every time.
    used_ops = {i.dxop for _, i in q.candidate_ops} | {i.dxop for _, i in q.committed_ops}
    conditional = [
        ({rq.CANDIDATE_WORLD_TO_OBJECT, rq.COMMITTED_WORLD_TO_OBJECT},
         'declare float @dx.op.worldToObject.f32(i32, i32, i8) #0'),
        ({rq.CANDIDATE_OBJECT_RAY_ORIGIN},
         'declare float @dx.op.objectRayOrigin.f32(i32, i8) #0'),
        ({rq.CANDIDATE_OBJECT_RAY_DIRECTION},
         'declare float @dx.op.objectRayDirection.f32(i32, i8) #0'),
    ]
    # Only reached from a generated hit shader. A raygen read of RayFlags
    # became a constant, so a shader reading it only there declares nothing,
    # and an unused declare is itself a validation error.
    if q.rayflags_in_loop:
        new += ['', '; Function Attrs: nounwind readnone',
                'declare i32 @dx.op.rayFlags.i32(i32) #0']
    for ops, decl in conditional:
        if used_ops & ops:
            new += ['', '; Function Attrs: nounwind readnone', decl]

    # The record read. Both are #2, nounwind readonly, copied from DXC output.
    #
    # cbufferLoadLegacy may ALREADY be declared, because the application's own
    # shader very likely has a cbuffer of its own; declaring it twice is as much
    # an error as declaring it unused.
    if q.needs_record_constants:
        new += ['', '; Function Attrs: nounwind readonly',
                'declare %%dx.types.Handle @%s(i32, %s) #2'
                % (RECORD_HANDLE_FN, RECORD_TYPE)]
        if '@dx.op.cbufferLoadLegacy.i32(' not in text:
            new += ['', '; Function Attrs: nounwind readonly',
                    'declare %s @dx.op.cbufferLoadLegacy.i32'
                    '(i32, %%dx.types.Handle, i32) #2' % CBRET]
    if q.needs_intersection:
        new += ['', '; Function Attrs: nounwind',
                'declare i1 @dx.op.reportHit.%s(i32, float, i32, %s*) #1'
                % (ATTRS[1:], ATTRS)]
    # Unconditional now: the generated any-hit may or may not exist, but
    # AnyHitNull always does and it is nothing but an IgnoreHit.
    new += ['', '; Function Attrs: noreturn nounwind',
            'declare void @dx.op.ignoreHit(i32) #3']
    if binding:
        # One overload serves every resource in the 6.6 form, because the
        # global is a handle whatever the resource is.
        new += ['', '; Function Attrs: nounwind readonly',
                'declare %s @dx.op.createHandleForLib.dx.types.Handle'
                '(i32, %s) #2' % (HANDLE_TYPE, HANDLE_TYPE)]
    else:
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
    if 'attributes #3' not in text:
        text = text.replace('attributes #2 = { nounwind readonly }',
                            'attributes #2 = { nounwind readonly }\n'
                            'attributes #3 = { noreturn nounwind }')
    return text


def _closesthit(name, status, q):
    """The generated closest-hit, writing `status` as the committed kind.

    A both-kinds query needs TWO of these. A triangle hit reports
    COMMITTED_TRIANGLE_HIT and a procedural one
    COMMITTED_PROCEDURAL_PRIMITIVE_HIT, and one shader cannot say both,
    because it does not know which hit group resolved to it.
    """
    ch = ['define void @{ch}({pl}* noalias nocapture %p, {at}* nocapture readonly %attr) #1 {{'
          .format(ch=name, pl=PAYLOAD, at=ATTRS),
          '  %t = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()',
          '  %ap = getelementptr inbounds {at}, {at}* %attr, i32 0, i32 0'.format(at=ATTRS),
          '  %b = load <2 x float>, <2 x float>* %ap, align 4',
          '  %pt = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 0'.format(pl=PAYLOAD),
          '  store float %t, float* %pt, align 4',
          '  %pb = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 1'.format(pl=PAYLOAD),
          '  store <2 x float> %b, <2 x float>* %pb, align 4',
          '  %ph = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 2'.format(pl=PAYLOAD),
          '  store i32 %d, i32* %%ph, align 4' % status]
    # The index fields. Emitted unconditionally: unused ones cost a dead call
    # the driver removes, and making them conditional is another place to get
    # the payload layout wrong.
    for idx, (ty, call) in sorted(CH_SOURCE.items()):
        ch.append('  %%id%d = %s' % (idx, call))
        ch.append('  %%pi%d = getelementptr inbounds %s, %s* %%p, i32 0, i32 %d'
                  % (idx, PAYLOAD, PAYLOAD, idx))
        ch.append('  store %s %%id%d, %s* %%pi%d, align 4' % (ty, idx, ty, idx))

    # The record's two numbers, when the shader asked for either. Both are
    # stored whenever either is read: one cbuffer load answers both, so
    # splitting them would cost a branch and save nothing.
    if q.needs_record_constants:
        ch += _record_read('g', 0, '%rq.geo')
        ch.append('  %%rq.gp = getelementptr inbounds %s, %s* %%p, i32 0, i32 5'
                  % (PAYLOAD, PAYLOAD))
        ch.append('  store i32 %rq.geo, i32* %rq.gp, align 4')
        ch.append('  %%rq.con = extractvalue %s %%rq.cbrg, 1' % CBRET)
        ch.append('  %%rq.cp = getelementptr inbounds %s, %s* %%p, i32 0, i32 10'
                  % (PAYLOAD, PAYLOAD))
        ch.append('  store i32 %rq.con, i32* %rq.cp, align 4')

    # Twelve fetches and twelve stores, so they are emitted ONLY when the
    # shader actually reads the matrix. The payload field exists either way,
    # because a layout that changes shape is a layout that gets an offset
    # wrong; it is the per-invocation work that is worth avoiding.
    if any(i.dxop == rq.COMMITTED_WORLD_TO_OBJECT for _, i in q.committed_ops):
        ch.append('  %pw = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 8'
                  .format(pl=PAYLOAD))
        for r in range(3):
            for c in range(4):
                s = r * 4 + c
                ch.append('  %%w%d = call float @dx.op.worldToObject.f32'
                          '(i32 152, i32 %d, i8 %d)  ; WorldToObject(row,col)'
                          % (s, r, c))
                ch.append('  %%pw%d = getelementptr inbounds [12 x float], '
                          '[12 x float]* %%pw, i32 0, i32 %d' % (s, s))
                ch.append('  store float %%w%d, float* %%pw%d, align 4' % (s, s))
    ch += ['  ret void', '}']
    return '\n'.join(ch)


def _append_shaders(text, module, q, exports, table, globals_, binds=None):
    # q is used below to decide whether the closest-hit fetches the matrix.
    """Emit the generated shader set after the raygen."""
    fns = []
    if q.needs_both:
        # ONE loop body, TWO shaders. The substitution of CandidateType is what
        # separates them: folded to CANDIDATE_PROCEDURAL_PRIMITIVE the triangle
        # arm dies and this is an intersection shader, folded to
        # CANDIDATE_NON_OPAQUE_TRIANGLE the procedural arm dies and it is an
        # any-hit. Each keeps the other's dead arm as valid but unreachable IR.
        fns.append(_intersection(module, q, exports, table, globals_, binds))
        fns.append(_anyhit(module, q, exports, table, globals_, binds))
    elif q.needs_intersection:
        fns.append(_intersection(module, q, exports, table, globals_, binds))
    elif q.loop:
        fns.append(_anyhit(module, q, exports, table, globals_, binds))

    if q.needs_both:
        # And two closest-hits, because the committed status differs and a
        # closest-hit cannot tell which hit group resolved to it.
        fns.append(_closesthit(exports['closesthit'], 1, q))
        fns.append(_closesthit(exports['closesthitproc'], 2, q))
    else:
        fns.append(_closesthit(exports['closesthit'],
                               2 if q.needs_intersection else 1, q))
    fns.append('''define void @{ms}({pl}* noalias nocapture %p) #1 {{
  %ph = getelementptr inbounds {pl}, {pl}* %p, i32 0, i32 2
  store i32 0, i32* %ph, align 4
  ret void
}}'''.format(ms=exports['miss'], pl=PAYLOAD))

    # The two never-commit stubs. Shapes taken from DXC, see
    # phase5/cases/reference/lib_null_ref.hlsl, which also confirms both are
    # SFI0=0x0 and so carry no Tier 1.1 feature flag.
    #
    # Rejecting every candidate is how a TRIANGLES hit group produces no hit:
    # traversal carries on past that geometry as if the shader had never seen
    # it. #3 is noreturn nounwind, as DXC marks its own.
    fns.append('''define void @{ah}({pl}* noalias nocapture %p, {at}* nocapture readnone %attr) #3 {{
  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()
  unreachable
}}'''.format(ah=exports['anyhitnull'], pl=PAYLOAD, at=ATTRS))

    # And reporting nothing is how a PROCEDURAL hit group produces no hit: an
    # intersection shader that returns has found nothing.
    fns.append('''define void @{is}() #1 {{
  ret void
}}'''.format(**{'is': exports['isectnull']}))

    # Insert after the line that closes the entry function. Done on lines
    # rather than with a multiline regex: `\s*$` there also eats the following
    # newlines, which produced a run of blank lines by accident. It is also the
    # one construct MSVC's std::regex cannot do, and the C++ port has to match
    # this byte for byte.
    lines = text.split('\n')
    at = next((i for i, l in enumerate(lines) if re.match(r'^\}\s*$', l)), None)
    if at is None:
        raise LowerError('cannot find the end of the entry function')
    block = ('\n' + '\n\n'.join(fns)).split('\n')
    lines[at + 1:at + 1] = block
    return '\n'.join(lines)


def _intersection(module, q, exports, table, globals_, binds=None):
    types = _type_names(module.text)
    """The Proceed loop body, re-rooted as an INTERSECTION shader.

    The same body as the any-hit case, with one substitution changed: where
    an any-hit folds `CandidateType()` to CANDIDATE_NON_OPAQUE_TRIANGLE (0),
    this folds it to CANDIDATE_PROCEDURAL_PRIMITIVE (1). The triangle branch
    then becomes dead and the procedural branch survives, and the transplant
    is otherwise identical. One body, two substitutions, two shaders.

    `CommitProceduralPrimitiveHit(t)` becomes `ReportHit(t, 0, attrs)`. An
    intersection shader has no accept or reject terminator: reporting IS
    accepting, and simply returning reports nothing, so every path out of the
    loop body becomes a plain `ret void`.
    """
    fn = q.fn
    header, latch, body = q.loop

    subst = {}
    for label in body:
        if label == latch:
            continue
        for i in fn.block(label).instrs:
            if i.dxop == rq.CANDIDATE_TYPE:
                subst[i.result] = '1'          # CANDIDATE_PROCEDURAL_PRIMITIVE
            elif i.dxop == rq.CANDIDATE_PROC_NON_OPAQUE:
                # Only non-opaque procedural primitives ever reach a Proceed
                # loop, so this is a tautology here, as CandidateType is.
                subst[i.result] = 'true'
            elif i.dxop == rq.CANDIDATE_BARY:
                # Triangle barycentrics have NO source in an intersection
                # shader: there is no attributes parameter to read them from.
                # This only appears in the triangle arm, which CandidateType
                # has just folded away, so it is dead. A constant keeps the IR
                # valid without pretending to a value. Substituted in the same
                # pre-pass as the others, so it does not depend on the order
                # blocks happen to be emitted in.
                subst[i.result] = '0.000000e+00'
            elif i.dxop == rq.CANDIDATE_FRONT_FACE:
                # Likewise: HitKind() is not available in an intersection
                # shader, and this is in the dead triangle arm.
                subst[i.result] = 'false'

    order = [header] + sorted(l for l in body if l not in (header, latch))
    out = ['define void @%s() #1 {' % exports['intersection'],
           '  %%rq.at = alloca %s, align 8' % ATTRS]

    used, inBody = set(), set()
    for label in body:
        if label != latch:
            for i in fn.block(label).instrs:
                used.update(_operands(i, types))
                if i.result:
                    inBody.add(i.result)
    # Everything the body reads from outside itself, rebuilt here in source
    # order. A resource handle becomes its library form; a thread id becomes
    # DispatchRaysIndex, which is the SAME ray so the same value; everything
    # else is pure and is emitted as it stood. _check_loop_isolated has already
    # refused anything not on that list.
    for i in _needed_chain(fn, _recomputable(fn, types), used, types, inBody):
        if i.dxop in (57, BIND_HANDLE):
            out.append(_handle_text(i, table, globals_, binds))
        elif i.dxop == 93:
            comp = re.match(r'i32\s+(\d+)', i.args[1].strip()).group(1)
            out.append('  %s = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 %s)'
                       '  ; DispatchRaysIndex(col)' % (i.result, comp))
        else:
            out.append(i.line)

    for label in order:
        blk = fn.block(label)
        if label != header:
            out.append('')
            out.append('%s:' % label[1:])
        for i in blk.instrs:
            if i.dxop in (rq.CANDIDATE_TYPE, rq.CANDIDATE_PROC_NON_OPAQUE,
                          rq.CANDIDATE_BARY, rq.CANDIDATE_FRONT_FACE):
                continue
            if i.dxop == rq.COMMIT_NON_OPAQUE:
                # A TRIANGLE commit, in the arm CandidateType folded away.
                # Dead, and an intersection shader has nothing to lower it
                # onto, so it simply goes.
                continue
            if i.dxop == rq.COMMIT_PROCEDURAL:
                t = rq_operand(i.args[2])
                out.append('  %%rq.rh%s = call i1 @dx.op.reportHit.%s'
                           '(i32 158, float %s, i32 0, %s* nonnull %%rq.at)'
                           '  ; ReportHit(THit,HitKind,Attributes)'
                           % (i.index, ATTRS[1:], t, ATTRS))
                continue
            if i.dxop == rq.ABORT:
                # Nothing to set: an intersection shader that returns has
                # reported nothing, and there is no later invocation to gate.
                continue
            if i.dxop in RECORD_WORD:
                # The record answers, because nothing in the shader can. The
                # local root signature bound this record's own constants.
                out += _record_read(i.result[1:], RECORD_WORD[i.dxop], i.result)
                continue
            if i.dxop in CANDIDATE_MAP:
                ty, callee, op, extra = CANDIDATE_MAP[i.dxop]
                args = ['i32 %d' % op] + [a.strip() for a in i.args[2:2 + extra]]
                out.append('  %s = call %s @%s(%s)'
                           % (i.result, ty, callee, ', '.join(args)))
                continue
            line = i.line
            for old, new in subst.items():
                line = re.sub(re.escape(old) + r'\b', new, line)
            # Every way out of the loop body is just a return here.
            line = re.sub(r'label\s+' + re.escape(latch) + r'\b', 'label %rq.done', line)
            for s2 in Block_successors(line):
                if s2 not in body and s2 != '%rq.done':
                    raise rq.Unsupported(
                        'Proceed loop body branches to %s, outside the loop; '
                        'no lowering is defined for that' % s2)
            out.append(line)

    out += ['', 'rq.done:', '  ret void', '}']
    return '\n'.join(out)


def rq_operand(arg):
    """The value part of an operand, e.g. 'float %v12' -> '%v12'."""
    from dxil import operand_name
    n = operand_name(arg)
    return n if n else arg.strip().split()[-1]


def _anyhit(module, q, exports, table, globals_, binds=None):
    types = _type_names(module.text)
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
            elif i.dxop == rq.CANDIDATE_PROC_NON_OPAQUE:
                # Meaningless for a triangle candidate, and in the arm
                # CandidateType has just folded away. Dead, but it still has to
                # be a value.
                subst[i.result] = 'false'

    # Abort() has no direct DXR 1.0 equivalent. An any-hit shader can only
    # IgnoreHit (reject and CONTINUE) or AcceptHitAndEndSearch (accept and
    # stop); there is no "reject and stop", which is what a bare Abort needs.
    #
    # So it becomes a payload flag, which covers both shapes uniformly. Abort
    # sets it, and every later any-hit invocation ignores its candidate
    # immediately. A commit followed by an abort still accepts, because the
    # control flow still falls through; a bare abort still rejects. In both
    # cases nothing further is committed, which is what stopping traversal
    # means for the result.
    #
    # Traversal itself carries on, so this is slower than the ideal.
    # AcceptHitAndEndSearch would be exact for the commit-then-abort case, but
    # only after proving the commit dominates the abort in the same iteration,
    # and correctness comes first here.
    aborts = bool(q.aborts)

    order = [header] + sorted(l for l in body if l not in (header, latch))
    out = ['define void @%s(%s* noalias nocapture %%p, %s* nocapture readonly %%attr) #1 {'
           % (exports['anyhit'], PAYLOAD, ATTRS),
           '  %rq.ap = getelementptr inbounds {at}, {at}* %attr, i32 0, i32 0'.format(at=ATTRS),
           '  %rq.ab = load <2 x float>, <2 x float>* %rq.ap, align 4']

    # Recreate every resource handle the body uses, under the SAME SSA name it
    # had in the raygen, so the transplanted instructions need no rewriting.
    used, inBody = set(), set()
    for label in body:
        if label != latch:
            for i in fn.block(label).instrs:
                used.update(_operands(i, types))
                if i.result:
                    inBody.add(i.result)
    # Everything the body reads from outside itself, rebuilt here in source
    # order. A resource handle becomes its library form; a thread id becomes
    # DispatchRaysIndex, which is the SAME ray so the same value; everything
    # else is pure and is emitted as it stood. _check_loop_isolated has already
    # refused anything not on that list.
    for i in _needed_chain(fn, _recomputable(fn, types), used, types, inBody):
        if i.dxop in (57, BIND_HANDLE):
            out.append(_handle_text(i, table, globals_, binds))
        elif i.dxop == 93:
            comp = re.match(r'i32\s+(\d+)', i.args[1].strip()).group(1)
            out.append('  %s = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 %s)'
                       '  ; DispatchRaysIndex(col)' % (i.result, comp))
        else:
            out.append(i.line)

    if aborts:
        out.append('  %%rq.pab = getelementptr inbounds %s, %s* %%p, i32 0, i32 9'
                   % (PAYLOAD, PAYLOAD))
        out.append('  %rq.abv = load i32, i32* %rq.pab, align 4')
        out.append('  %rq.abc = icmp ne i32 %rq.abv, 0')
        out.append('  br i1 %rq.abc, label %rq.reject, label %rq.body')
        out.append('')
        out.append('rq.body:')

    for label in order:
        blk = fn.block(label)
        if label != header:
            out.append('')
            out.append('%s:' % label[1:])
        for i in blk.instrs:
            if i.dxop in (rq.CANDIDATE_TYPE, rq.COMMIT_NON_OPAQUE,
                          rq.CANDIDATE_PROC_NON_OPAQUE):
                continue
            if i.dxop == rq.COMMIT_PROCEDURAL:
                # A PROCEDURAL commit, in the arm CandidateType folded away.
                # Dead, and an any-hit shader cannot report a procedural hit,
                # so it simply goes.
                continue
            if i.dxop == rq.ABORT:
                out.append('  store i32 1, i32* %rq.pab, align 4')
                continue
            if i.dxop == rq.CANDIDATE_BARY:
                comp = re.match(r'i8\s+(\d+)', i.args[2].strip()).group(1)
                out.append('  %s = extractelement <2 x float> %%rq.ab, i32 %s'
                           % (i.result, comp))
                continue
            if i.dxop == rq.CANDIDATE_FRONT_FACE:
                # RayQuery returns a bool; HitKind() is an integer.
                hk = '%%rq.hk%s' % i.result[1:]
                out.append('  %s = call i32 @dx.op.hitKind.i32(i32 143)'
                           '  ; HitKind()' % hk)
                out.append('  %s = icmp eq i32 %s, %d' % (i.result, hk, HIT_KIND_FRONT))
                continue
            if i.dxop in RECORD_WORD:
                # The record answers, because nothing in the shader can. The
                # local root signature bound this record's own constants.
                out += _record_read(i.result[1:], RECORD_WORD[i.dxop], i.result)
                continue
            if i.dxop in CANDIDATE_MAP:
                # Same operands as the RayQuery form, minus the query handle.
                ty, callee, op, extra = CANDIDATE_MAP[i.dxop]
                args = ['i32 %d' % op] + [a.strip() for a in i.args[2:2 + extra]]
                out.append('  %s = call %s @%s(%s)'
                           % (i.result, ty, callee, ', '.join(args)))
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


def _rewrite_metadata(text, md, table, globals_, q, exports, binding=False):
    """Point the resource records at the globals and rebuild the entry points."""
    for (cls, nid), (sym, gty, elem, name) in globals_.items():
        fields = [f.strip() for f in md[nid].split(',')]
        if binding:
            # `%CB* bitcast (%dx.types.Handle* @CB to %CB*)`, copied from DXC.
            # The global holds a handle; the record still has to name the
            # resource type, so it casts.
            fields[1] = ('%s* bitcast (%s* @%s to %s*)'
                         % (gty, HANDLE_TYPE, sym, gty))
        else:
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

    # The record cbuffer, added to whatever the application already had.
    #
    # Shape copied from DXC, not invented: a cbuffer record is
    # {id, global, name, space, lowerBound, rangeSize, sizeInBytes, extra}.
    # space 1 keeps it clear of anything the application's own global root
    # signature binds, which the shim must not disturb. See
    # phase5/cases/reference/lib_localroot_ref.hlsl.
    if q.needs_record_constants:
        groups = [g.strip() for g in md[int(res)].split(',')]
        while len(groups) < 4:
            groups.append('null')
        have = []
        if groups[2] != 'null':
            have = [x.strip() for x in md[int(groups[2][1:])].split(',')]
        rec = node('i32 %d, %s* %s, !"rq_record", i32 1, i32 0, i32 1, i32 8, null'
                   % (len(have), RECORD_TYPE, RECORD_GLOBAL))
        groups[2] = '!%d' % node(', '.join(have + ['!%d' % rec]))
        # Rewritten in place, so !dx.resources keeps pointing at the same node
        # and nothing else has to learn a new id.
        text = re.sub(r'^!%s = !\{.*\}\s*$' % res,
                      '!%s = !{%s}' % (res, ', '.join(groups)), text, flags=re.M)

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
    if q.needs_both:
        # An intersection shader is void(), so it annotates like the raygen.
        ann += ['void ()* @%s' % exports['intersection'], '!%d' % ann_raygen]
        ann += ['void %s* @%s' % (sig_h, exports['anyhit']), '!%d' % ann_hit]
    elif q.needs_intersection:
        # An intersection shader is void(), so it annotates like the raygen.
        ann += ['void ()* @%s' % exports['intersection'], '!%d' % ann_raygen]
    elif q.loop:
        ann += ['void %s* @%s' % (sig_h, exports['anyhit']), '!%d' % ann_hit]
    ann += ['void %s* @%s' % (sig_h, exports['closesthit']), '!%d' % ann_hit]
    if q.needs_both:
        ann += ['void %s* @%s' % (sig_h, exports['closesthitproc']),
                '!%d' % ann_hit]
    ann += ['void %s* @%s' % (sig_p, exports['miss']), '!%d' % ann_miss]
    ann += ['void %s* @%s' % (sig_h, exports['anyhitnull']), '!%d' % ann_hit]
    # An intersection shader is void(), so it annotates like the raygen.
    ann += ['void ()* @%s' % exports['isectnull'], '!%d' % ann_raygen]
    ann_id = node(', '.join(ann))

    # Entry points: a resource-only record, then one per export. Tags are
    # 8 shader kind, 6 payload bytes, 7 attribute bytes, 5 auto binding space.
    zero = node('i32 0')
    flags = node('i32 0, i64 %d' % (_module_flags(text, md) & ~RT11_SHADER_FLAG))
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

    if q.needs_both:
        eps.append(entry(exports['intersection'], '()', 8, False, False))
        eps.append(entry(exports['anyhit'], sig_h, 9, True, True))
    elif q.needs_intersection:
        # Shader kind 8. An intersection shader carries neither a payload size
        # nor an attribute size, exactly as DXC emits it.
        eps.append(entry(exports['intersection'], '()', 8, False, False))
    elif q.loop:
        eps.append(entry(exports['anyhit'], sig_h, 9, True, True))
    eps.append(entry(exports['closesthit'], sig_h, 10, True, True))
    if q.needs_both:
        eps.append(entry(exports['closesthitproc'], sig_h, 10, True, True))
    eps.append(entry(exports['miss'], sig_p, 11, True, False))
    eps.append(entry(exports['anyhitnull'], sig_h, 9, True, True))
    eps.append(entry(exports['isectnull'], '()', 8, False, False))
    eps.append(entry(exports['raygen'], '()', 7, False, False))

    # Allocated LAST, so a shader that does not index dynamically keeps every
    # other node id exactly where it was.
    if '!RQNU' in text:
        text = text.replace('!RQNU', '!%d' % node('i32 1'))

    text = text.replace('!dx.entryPoints = !{!%d}' % old_entry,
                        '!dx.typeAnnotations = !{!%d}\n!dx.entryPoints = !{%s}'
                        % (ann_id, ', '.join('!%d' % e for e in eps)))
    # The old compute entry record and its numthreads node are dead.
    text = re.sub(r'^!%d = !\{void \(\)\* @\w+.*\}\s*$' % old_entry, '', text, flags=re.M)
    return text.rstrip('\n') + '\n' + '\n'.join(nodes) + '\n'
