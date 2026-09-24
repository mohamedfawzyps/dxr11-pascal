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

from dxil import operand_name, render, split_args
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
# either, which is exactly why they were refused. They come out of a RAW BUFFER
# bound by a LOCAL root signature root SRV at t0, space1, so the value is per
# hit-group-record and the shim chose it when it built the table.
#
# In a Shader Model 6.6 module the handle MUST be annotated, and that is what
# the Pascal driver crash always was. A 6.6 library whose hit shader reaches a
# LOCAL root signature resource through a bare createHandleForLib, with no
# annotateHandle after it, crashes the driver inside CreateStateObject on
# about half of all cold compiles. Measured one variable at a time on the
# vendored Unreal library (phase5/cases/driver-crash/):
#
#     cbuffer record, unannotated (0.36.x)   9 of 15     annotated   0 of 15
#     raw buffer record, unannotated         10 of 15    annotated   0 of 15
#
# So it was never the cbuffer, and never reading the local root signature as
# such: 0.37.0 to 0.39.x baked the values in to avoid a read that only needed
# annotating. The raw buffer stays because it is built and tested, and each
# record carries its own pair with no cap on distinct pairs.
#
# Shapes copied from DXC, phase5/cases/reference/lib_localsrv_ref.hlsl: at
# lib_6_6 a %dx.types.Handle global, createHandleForLib on it, annotateHandle
# as a raw buffer (kind 11); at lib_6_5, which has no annotateHandle, the
# struct global and a bare createHandleForLib.
RECORD_TYPE = '%rq_record'
RECORD_GLOBAL = '@rq_record'
RECORD_HANDLE_FN = 'dx.op.createHandleForLib.rq_record'
RESRET = '%dx.types.ResRet.i32'
# Shader flag for raw and structured buffers. The validator requires it once a
# module reads one, and Unreal's modules declare it clear.
RAW_BUFFER_FLAG = 0x10
# Which word of the record each accessor reads.
RECORD_WORD = {
    rq.CANDIDATE_GEOMETRY_INDEX: 0,
    rq.COMMITTED_GEOMETRY_INDEX: 0,
    rq.CANDIDATE_INSTANCE_CONTRIB: 1,
    rq.COMMITTED_INSTANCE_CONTRIB: 1,
}


def _is_sm66(text):
    """Shader Model 6.6 or later, where every handle is annotated."""
    m = re.search(r'^!\d+ = !\{!"(?:cs|lib)", i32 (\d+), i32 (\d+)\}', text, re.M)
    return bool(m) and (int(m.group(1)), int(m.group(2))) >= (6, 6)


def _record_read(tag, word, result, sm66=False):
    """The four lines that pull one word out of the hit group record.

    Emitted per use rather than hoisted. The handle and the load are pure and
    the driver folds the duplicates; hoisting them by hand would mean finding a
    place to put them that dominates every use, which is a CFG question this
    pass has no reason to ask."""
    if sm66:
        handle = [
            '  %%rq.cbv%s = load %s, %s* %s, align 4'
            % (tag, HANDLE_TYPE, HANDLE_TYPE, RECORD_GLOBAL),
            '  %%rq.cbh%s.lib = call %s @dx.op.createHandleForLib.dx.types.Handle'
            '(i32 160, %s %%rq.cbv%s)  ; CreateHandleForLib(Resource)'
            % (tag, HANDLE_TYPE, HANDLE_TYPE, tag),
            '  %%rq.cbh%s = call %s @dx.op.annotateHandle(i32 216, %s %%rq.cbh%s.lib, '
            '%%dx.types.ResourceProperties { i32 11, i32 0 })'
            '  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer'
            % (tag, HANDLE_TYPE, HANDLE_TYPE, tag),
        ]
    else:
        handle = [
            '  %%rq.cbv%s = load %s, %s* %s, align 4'
            % (tag, RECORD_TYPE, RECORD_TYPE, RECORD_GLOBAL),
            '  %%rq.cbh%s = call %%dx.types.Handle @%s(i32 160, %s %%rq.cbv%s)'
            '  ; CreateHandleForLib(Resource)' % (tag, RECORD_HANDLE_FN, RECORD_TYPE, tag),
        ]
    return handle + [
        '  %%rq.cbr%s = call %s @dx.op.rawBufferLoad.i32(i32 139, '
        '%%dx.types.Handle %%rq.cbh%s, i32 0, i32 undef, i8 3, i32 4)'
        '  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)'
        % (tag, RESRET, tag),
        '  %s = extractvalue %s %%rq.cbr%s, %d' % (result, RESRET, tag, word),
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
    # How the record handle is written; see RECORD_TYPE.
    queries = [q] + q.others
    # One table serves every trace, so all of them use the geometry multiplier
    # if any query reads record data; the closest-hit fills what any reads.
    rc = any(x.needs_record_constants for x in queries)
    fields = {}
    base = CARRY_FIELD0 + (1 if len(queries) > 1 else 0)
    for x in queries:
        x.record_sm66 = _is_sm66(text)
        x.needs_record_constants = rc
        x.carry = []
    globals_ = _plan_globals(table)
    _edit_resources(module, q, edits, table, globals_, binds)
    _edit_ray_index(module, q, edits)
    for x in queries:
        _edit_query(module, x, edits, exports, fields, base, len(queries) > 1)
    q.carry_fields = fields

    fn = q.fn
    edits[fn.index] = 'define void @%s() #RQNW {' % exports['raygen']

    out = render(module, edits)
    out = _add_types_and_globals(out, globals_, q.needs_record_constants, binding,
                                 q.record_sm66, _payload_type(q))
    out = _swap_declarations(out, q, globals_, binding)
    out = _append_shaders(out, module, q, exports, table, globals_, binds)
    out = _rewrite_metadata(out, md, table, globals_, q, exports, binding)
    return _resolve_attrs(out)


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
HEAP_HANDLE = 218
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


# Attribute group NUMBERS are a fact about the input module, not a constant.
#
# The lowering used to write #1 for nounwind, #2 for nounwind readonly and #3
# for noreturn nounwind, because that is how DXC numbered every shader this
# project had written. A real Unreal module numbers them differently:
#
#     ours     #0 readnone  #1 nounwind           #2 nounwind readonly  #3 added
#     Unreal   #0 readnone  #1 nounwind readonly  #2 nounwind           #3 ABSENT
#
# So AnyHit came out marked readonly, the declares came out marked nounwind,
# and #3 was never appended at all, because it was added by replacing the
# literal line `attributes #2 = { nounwind readonly }`, which that module does
# not contain. AnyHitNull was then marked #3, which resolved to nothing, so
# IgnoreHit was not noreturn and the `unreachable` after it was illegal:
#
#     error: Instructions must be of an allowed type.
#     note: at 'unreachable' in block '#0' of function 'AnyHitNull'.
#
# Three of six refusals in one real Unreal session, and the message points at
# the instruction rather than at the attribute that made it illegal.
#
# The generated text now writes placeholders and this resolves them against the
# module, reusing a group that already exists and appending one that does not.
# Ours still resolve to 1, 2 and 3, so no existing output moves by a byte.
ATTR_PLACEHOLDERS = [('#RQNONE', 'nounwind readnone'),
                     ('#RQNW', 'nounwind'),
                     ('#RQRO', 'nounwind readonly'),
                     ('#RQNR', 'noreturn nounwind')]


def _resolve_attrs(text):
    lines = text.split('\n')
    have = {}
    used = set()
    last = -1
    for n, line in enumerate(lines):
        m = re.match(r'attributes #(\d+) = \{ (.*) \}\s*$', line)
        if not m:
            continue
        used.add(int(m.group(1)))
        # First wins. A module with two groups of the same body is not
        # something DXC emits, but picking the same one twice is the safe
        # answer if it ever does.
        have.setdefault(m.group(2).strip(), int(m.group(1)))
        last = n
    wanted = [(ph, body) for ph, body in ATTR_PLACEHOLDERS if ph in text]
    added = []
    for ph, body in wanted:
        if body in have:
            continue
        nid = 0
        while nid in used:
            nid += 1
        used.add(nid)
        have[body] = nid
        added.append('attributes #%d = { %s }' % (nid, body))
    if added:
        if last < 0:
            raise LowerError('module declares no attribute groups')
        lines[last + 1:last + 1] = added
        text = '\n'.join(lines)
    for ph, body in wanted:
        text = text.replace(ph, '#%d' % have[body])
    return text


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


# The compute thread-index ops. None is legal in a ray tracing stage, and a
# raygen has no thread group at all. But the shim launches exactly
# groups * numthreads rays, one per thread, so DispatchRaysIndex IS the
# thread's SV_DispatchThreadID, and the other three follow from it and
# numthreads:
#
#   93  SV_DispatchThreadID.c  = DispatchRaysIndex.c
#   94  SV_GroupID.c           = DispatchRaysIndex.c / numthreads.c
#   95  SV_GroupThreadID.c     = DispatchRaysIndex.c % numthreads.c
#   96  SV_GroupIndex          = (gt.z * ny + gt.y) * nx + gt.x, gt the above
#
# Exact, not approximate. What a group actually SHARES, groupshared memory and
# barriers, is still refused by the analysis; these four are only arithmetic on
# which thread this is. Unreal's LumenRadianceCacheHardwareRayTracingCS reads
# 94 and 96 and nothing else of the group, and 24 of one Escher run's refusals
# were the validator rejecting exactly these in the raygen.
#
# The divisors are constants of at least 1, so the division the brief keeps off
# the recomputable list for fear of hoisting it past a guard cannot divide by
# zero here.
GROUP_ID = 94
THREAD_ID_IN_GROUP = 95
FLAT_THREAD_ID_IN_GROUP = 96
_THREAD_OPS = (93, GROUP_ID, THREAD_ID_IN_GROUP, FLAT_THREAD_ID_IN_GROUP)


NO_NUMTHREADS = ('the entry point declares no numthreads, so the thread group '
                 'index cannot be rebuilt')


def _numthreads(module):
    """(nx, ny, nz) from the compute entry point's properties, tag 4.

    Mirrors rq::NumThreads in proxy/rewriter/rq_analyze.cpp, and refuses with
    one message whatever is missing, because the C++ only answers yes or no."""
    text = module.text
    md = _metadata(text)
    m = re.search(r'!dx\.entryPoints\s*=\s*!\{!(\d+)\}', text)
    if not m or int(m.group(1)) not in md:
        raise LowerError(NO_NUMTHREADS)
    fields = split_args(md[int(m.group(1))])
    if len(fields) < 5 or not fields[4].startswith('!'):
        raise LowerError(NO_NUMTHREADS)
    props = split_args(md.get(int(fields[4][1:]), ''))
    for i in range(0, len(props) - 1, 2):
        if props[i] != 'i32 4' or not props[i + 1].startswith('!'):
            continue
        v = split_args(md.get(int(props[i + 1][1:]), ''))
        nums = [re.match(r'i32\s+(\d+)$', x) for x in v]
        if len(v) == 3 and all(nums):
            return tuple(int(n.group(1)) for n in nums)
        break
    raise LowerError(NO_NUMTHREADS)


def _thread_index_text(instr, module):
    """A compute thread-index op, rewritten over DispatchRaysIndex."""
    r = instr.result
    tag = r[1:]

    def dri(name, comp):
        return ('  %s = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 %d)'
                '  ; DispatchRaysIndex(col)' % (name, comp))

    if instr.dxop in (93, GROUP_ID, THREAD_ID_IN_GROUP):
        comp = int(re.match(r'i32\s+(\d+)', instr.args[1].strip()).group(1))
        if comp > 2:
            raise LowerError('thread index component %d out of range' % comp)
        if instr.dxop == 93:
            return dri(r, comp)
        nt = _numthreads(module)
        d = '%%rq.dri.%s' % tag
        op = 'udiv' if instr.dxop == GROUP_ID else 'urem'
        return '\n'.join([dri(d, comp), '  %s = %s i32 %s, %d' % (r, op, d, nt[comp])])

    nt = _numthreads(module)
    out = []
    for c, ax in enumerate('xyz'):
        out.append(dri('%%rq.dri.%s.%s' % (tag, ax), c))
        out.append('  %%rq.gt.%s.%s = urem i32 %%rq.dri.%s.%s, %d' % (tag, ax, tag, ax, nt[c]))
    out.append('  %%rq.gi.%s.a = mul i32 %%rq.gt.%s.z, %d' % (tag, tag, nt[1]))
    out.append('  %%rq.gi.%s.b = add i32 %%rq.gi.%s.a, %%rq.gt.%s.y' % (tag, tag, tag))
    out.append('  %%rq.gi.%s.c = mul i32 %%rq.gi.%s.b, %d' % (tag, tag, nt[0]))
    out.append('  %s = add i32 %%rq.gi.%s.c, %%rq.gt.%s.x' % (r, tag, tag))
    return '\n'.join(out)


def _edit_ray_index(module, q, edits):
    """No compute thread-index op is legal in a raygen; see _THREAD_OPS."""
    for block, instr in q.fn.instrs():
        if instr.dxop in _THREAD_OPS:
            edits[instr.index] = _thread_index_text(instr, module)


def _edit_query(module, q, edits, exports, fields, base, multi=False):
    """Replace the query with a payload and one TraceRay, and drop the loop."""
    fn = q.fn

    # The payload alloca has to be in the entry block.
    entry = fn.blocks[0]
    if not entry.instrs:
        raise LowerError('entry block is empty')
    first = entry.instrs[0]
    pending = edits.get(first.index, first.line)
    edits[first.index] = ('  %%%s.pl = alloca %s, align 8\n%s' % (q.pfx, PAYLOAD, pending))

    # The allocate disappears entirely; the handle it produced is only ever
    # used by ops this pass rewrites.
    edits[q.alloc.index] = None

    ra = q.ray_args
    exit_label = None
    if q.loop:
        header, latch, body = q.loop
        exit_label = _loop_exit(fn, header, latch, body)
        outside = _check_loop_isolated(fn, q, header, latch, body,
                                       _type_names(module.text))
        q.carry = _plan_carry(fn, q, outside, fields, base)
        _check_loop_side_effects(module, fn, q, body)

    edits[q.trace.index] = _trace_block(q, ra, exit_label, multi)

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


def _trace_block(q, ra, exit_label, multi=False):
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
        lines.append('  %%%s.pl%d = getelementptr inbounds %s, %s* %%%s.pl, i32 0, i32 %d'
                     % (q.pfx, idx, PAYLOAD, PAYLOAD, q.pfx, idx))
        lines.append('  store %s %s, %s* %%%s.pl%d, align %d'
                     % (ty, zero, ty, q.pfx, idx, align))
    if multi:
        # Which query this trace serves, read by the one any-hit they share.
        lines.append('  %%%s.plq = getelementptr inbounds %s, %s* %%%s.pl, i32 0, i32 %d'
                     % (q.pfx, PAYLOAD, PAYLOAD, q.pfx, QID_FIELD))
        lines.append('  store i32 %d, i32* %%%s.plq, align 4' % (q.qid, q.pfx))
    for name, ty, field in q.carry:
        lines.append('  %%%s.plc%d = getelementptr inbounds %s, %s* %%%s.pl, i32 0, i32 %d'
                     % (q.pfx, field, PAYLOAD, PAYLOAD, q.pfx, field))
        if ty == 'i1':
            lines.append('  %%%s.plz%d = zext i1 %s to i32' % (q.pfx, field, name))
            lines.append('  store i32 %%%s.plz%d, i32* %%%s.plc%d, align 4'
                         % (q.pfx, field, q.pfx, field))
        else:
            lines.append('  store %s %s, %s* %%%s.plc%d, align 4'
                         % (ty, name, ty, q.pfx, field))
    lines += flag_setup
    lines += [
        '  call void @dx.op.traceRay.%s(i32 157, %%dx.types.Handle %s, %s, '
        '%s, i32 0, i32 %d, i32 0, %s, %s, %s, %s, %s, %s, %s, %s, %s* nonnull %%%s.pl)'
        '  ; TraceRay(...)' % (
            PAYLOAD[1:], q.as_handle, flag_text, ra['mask'], geom_mult,
            ra['origin'][0], ra['origin'][1], ra['origin'][2], ra['tmin'],
            ra['direction'][0], ra['direction'][1], ra['direction'][2], ra['tmax'],
            PAYLOAD, q.pfx),
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
#
# 218 is createHandleFromHeap, the bindless form, and it was left off
# DELIBERATELY and wrongly. The 0.25.0 note said a heap handle used inside the
# Proceed loop is refused because 218 is not on this list, and recorded that as
# correct. It was measured on a shader that kept all 16 of its heap handles in
# the raygen, so nothing counted the in-loop case. Counting it: 58 of the 157
# shaders one Escher run refused are exactly this, which makes it the single
# largest refusal a real game produces.
#
# It is recomputable for the reason the other handles are, one step further
# out. The descriptor heap is set on the command list and is the same heap in
# the any-hit as in the raygen, and the index reaches it from a cbuffer, which
# holds the same bytes for the whole dispatch. So the hit shader can rebuild
# the handle rather than be handed it. The fixpoint below is what enforces
# "from a cbuffer": 218 is admitted only when its index is ALREADY
# recomputable, so an index computed from a UAV read or a phi still refuses.
#
# It needs no conversion either, unlike 57 and 217. A heap handle means the
# same thing in a library as in a compute shader, which the 0.25.0 measurement
# established, so it is emitted verbatim.
#
# 94, 95 and 96 are the group forms of 93, and the hit shader rebuilds them
# from DispatchRaysIndex exactly as the raygen does. See _THREAD_OPS.
_PURE_DXOP = {57, 59, 93, GROUP_ID, THREAD_ID_IN_GROUP, FLAT_THREAD_ID_IN_GROUP,
              ANNOTATE_HANDLE, BIND_HANDLE, HEAP_HANDLE}


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


# Loads, 0.42.0. A value read from a READ-ONLY buffer or texture before the
# loop and used inside it: the hit shader reads it again, and gets the same
# value, because a dispatch cannot write a resource it reads as an SRV: SRV and
# UAV access are exclusive resource states in D3D12. An out-of-range load
# returns zero rather than trapping, so hoisting one past a guard is safe in a
# way integer division is not.
#
# A load from a UAV is NOT re-read. Another thread may write that location
# during the dispatch, so a hit shader reading it on every candidate could see
# the old value on one and the new on the next, which the original, reading
# once, never can. Such a value travels in the PAYLOAD instead, see
# _plan_carry; that is exact whatever other threads do.
LOAD_OPS = (66, 68, 139)   # textureLoad, bufferLoad, rawBufferLoad
_UAV_BIT = 0x1000          # ResourceProperties, first word: IsUAV


def _handle_class(defs, name):
    """'srv', 'uav' or None, read from how the handle was made."""
    i = defs.get(name)
    if i is None:
        return None
    if i.dxop == ANNOTATE_HANDLE:
        m = re.search(r'%dx\.types\.ResourceProperties \{ i32 (\d+),', i.line)
        if not m:
            return None
        return 'uav' if int(m.group(1)) & _UAV_BIT else 'srv'
    if i.dxop == 57 and i.args:
        m = re.match(r'i8 (\d+)', i.args[1].strip()) if len(i.args) > 1 else None
    elif i.dxop == BIND_HANDLE:
        m = re.search(r'ResBind \{ i32 -?\d+, i32 -?\d+, i32 -?\d+, i8 (\d+) \}', i.line)
    else:
        return None
    if not m:
        return None
    return {'0': 'srv', '1': 'uav'}.get(m.group(1))


def _recomputable(fn, types):
    """{result -> instr} for every value the generated hit shader can rebuild.

    A fixpoint, because a chain is only recomputable if every link is."""
    ok = {}
    defs = {i.result: i for _, i in fn.instrs() if i.result}
    changed = True
    while changed:
        changed = False
        for _, i in fn.instrs():
            if not i.result or i.result in ok:
                continue
            if i.dxop in LOAD_OPS:
                h = i.args[1].split()[-1] if len(i.args) > 1 else None
                cls = _handle_class(defs, h)
                if h not in ok or cls != 'srv':
                    continue
            elif i.dxop is not None and i.dxop not in _PURE_DXOP:
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


def _attr_groups(module):
    """callee name -> the attribute group TEXT its declaration carries.

    DXC marks every dx.op declaration with one of three groups, and the module
    says which is which: `readnone` is pure, `readonly` loads, and a bare
    `nounwind` WRITES MEMORY. Reading it from the module beats a hand-kept list
    of store opcodes, which would be incomplete the day DXIL grows another one.
    The numbering is not fixed either, see the attribute group bug in 0.26.0,
    so the groups are resolved per module rather than assumed."""
    attrs = {}
    for line in module.lines:
        m = re.match(r'^attributes\s+#(\d+)\s*=\s*\{(.*)\}\s*$', line)
        if m:
            attrs[m.group(1)] = m.group(2)
    out = {}
    for line in module.lines:
        m = re.match(r'^declare\s+.*@([\w.$]+)\s*\(.*\)\s*#(\d+)\s*$', line)
        if m:
            out[m.group(1)] = attrs.get(m.group(2), '')
    return out


BUFFER_UPDATE_COUNTER = 70
# Stores whose index operand, argument 2, can be a counter's result.
APPEND_STORES = (69, 140)   # bufferStore, rawBufferStore


def _check_loop_side_effects(module, fn, q, body):
    """A Proceed loop body that WRITES is not transplantable, with one
    exception: an APPEND.

    The exception, 0.41.0. Unreal's shared TraceRayInline wrapper appends a
    debug record per candidate, which put 47 MegaLights and Lumen shaders
    behind this refusal. An append is a counter update and stores indexed by
    the value it returned. Its result does not depend on the ORDER candidates
    arrive in, which is undefined for RayQuery and for any-hit alike, only on
    how MANY arrive. And that is the part the proxy makes equal: it sets
    D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION on every
    bottom-level geometry, so the any-hit runs at most once per intersection,
    as Proceed() yields each candidate once. Still refused around an append:
    Abort(), after which this lowering lets traversal continue and would
    append records RayQuery would not; and an intersection shader, which the
    spec lets run more than once per primitive whatever the flags say.

    The isolation check above guards what the body READS. Nothing guarded what
    it writes, and the two are not the same question. The any-hit shader runs a
    different number of times than the loop body does, by design:

      - with RAY_FLAG_FORCE_OPAQUE no candidate is ever yielded, so the any-hit
        never runs and the writes simply do not happen;
      - with FORCE_NON_OPAQUE it runs once per candidate in an
        implementation-defined order, so both the count and the order move.

    So a store, an append or an atomic in that body means something different
    after lowering, silently. Found on RayTracingDebugMainCS, a real Unreal
    shader that appends a debug record per candidate: it lowered, validated,
    signed, and then killed the device inside CreateStateObject. Whether the
    write is also what the driver choked on is NOT established, and this
    refusal does not rest on it.

    Every shader in this suite has a pure loop body, which is why nothing here
    could expose it. Fourth time a suite of cases written to demonstrate a
    lowering has shared its author's blind spot.

    LIMIT, stated rather than hidden: this reads dx.op calls. A plain `store`
    reaches only an alloca or groupshared in practice, and groupshared around a
    query is already refused."""
    groups = _attr_groups(module)
    writes, counters = [], set()
    for label in body:
        for i in fn.block(label).instrs:
            if not i.callee or not i.callee.startswith('dx.op.'):
                continue
            # The RayQuery ops themselves are mutators and are all declared
            # nounwind. They are not transplanted, they are REWRITTEN, and the
            # analysis already refuses any rayQuery opcode it does not know.
            if (i.callee.startswith('dx.op.rayQuery_')
                    or i.callee == 'dx.op.allocateRayQuery'):
                continue
            attr = groups.get(i.callee)
            if attr is not None and ('readnone' in attr or 'readonly' in attr):
                continue
            writes.append(i)
            if i.dxop == BUFFER_UPDATE_COUNTER and i.result:
                counters.add(i.result)
    for i in writes:
        if i.dxop == BUFFER_UPDATE_COUNTER:
            continue
        if (i.dxop in APPEND_STORES and len(i.args) > 2
                and i.args[2].split()[-1] in counters):
            continue
        raise rq.Unsupported(
            'Proceed loop body has a side effect (%s, opcode %s); the '
            'any-hit shader it becomes runs a different number of times '
            'than the loop body does, and in an implementation-defined '
            'order, so the write would not be the same write'
            % (i.callee, i.dxop if i.dxop is not None else '?'))
    if writes and q.aborts:
        raise rq.Unsupported(
            'Proceed loop body appends to a buffer and the query calls Abort(); '
            'this lowering lets traversal continue after an abort, so it would '
            'append records RayQuery would not')
    if writes and q.needs_intersection:
        raise rq.Unsupported(
            'Proceed loop body appends to a buffer and becomes an intersection '
            'shader, which may run more than once per primitive whatever the '
            'geometry flags say')


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
    outside = sorted(u for u in used
                     if u not in defined and u != q.handle and u not in handles)
    for b, i in fn.instrs():
        if b.label in body:
            continue
        for u in _operands(i, types):
            if u in defined:
                raise rq.Unsupported(
                    'value %s defined in the Proceed loop is used after it; '
                    'the any-hit shader cannot return it' % u)
    return outside


# Values the loop body reads from before it and the hit shader cannot rebuild,
# 0.42.0: they travel in the PAYLOAD. The raygen stores each one just before
# TraceRay and the any-hit loads it back at entry under its original name, so
# the loop body needs no rewriting. Exact, since it is the value the raygen
# computed, read once, whatever any other thread writes meanwhile.
#
# 24 of Unreal's MegaLights shaders need exactly one: a light sample field read
# from RWLightSamples before the trace. The fields go AFTER the fixed ones, so
# nothing else moves, and the payload grows 4 bytes per value.
#
# Refused, and each is named: a value defined after the trace (the raygen
# cannot store what it has not computed yet); a type other than i32, float or
# i1; more than MAX_CARRY values; and a loop body that becomes an INTERSECTION
# shader, which has no payload in DXR 1.0.
CARRY_FIELD0 = 11
MAX_CARRY = 16
# With several queries, field 11 says which one a trace serves and the carried
# values start at 12.
QID_FIELD = 11
_CARRY_TYPES = ('i32', 'float', 'i1')
_FLAGS = {'nuw', 'nsw', 'exact', 'fast', 'nnan', 'ninf', 'nsz', 'arcp', 'contract',
          'afn', 'reassoc'}
_RET_SUFFIX = {'i32': 'i32', 'f32': 'float', 'i16': 'i16', 'f16': 'half',
               'i64': 'i64', 'f64': 'double'}


def _result_type(i):
    """The LLVM type of the value an instruction defines, or None."""
    body = i.body
    cut = body.find(';')
    if cut >= 0:
        body = body[:cut]
    words = body.split()
    if not words:
        return None
    op = words[0]
    if op in ('icmp', 'fcmp'):
        return 'i1'
    if op in ('call', 'load', 'phi'):
        rest = [w for w in words[1:] if w not in _FLAGS]
        return rest[0].rstrip(',') if rest else None
    if op == 'select':
        m = re.match(r'select\s+i1\s+[^,]+,\s+(\S+)\s', body)
        return m.group(1) if m else None
    m = re.search(r'\sto\s+(\S+)\s*$', body.strip())
    if op in ('zext', 'sext', 'trunc', 'bitcast', 'sitofp', 'uitofp', 'fptosi',
              'fptoui', 'fpext', 'fptrunc') and m:
        return m.group(1)
    if op == 'extractvalue':
        m = re.match(r'extractvalue\s+%dx\.types\.(ResRet|CBufRet)\.(\w+)\s+\S+,\s*(\d+)', body)
        if not m:
            return None
        if m.group(1) == 'ResRet' and m.group(3) == '4':
            return 'i32'
        return _RET_SUFFIX.get(m.group(2))
    if op == 'extractelement':
        m = re.match(r'extractelement\s+<\d+ x (\S+)>', body)
        return m.group(1) if m else None
    rest = [w for w in words[1:] if w not in _FLAGS]
    return rest[0] if rest else None


def _plan_carry(fn, q, outside, fields=None, base=CARRY_FIELD0):
    """[(name, type, field)] for the values that travel in the payload.

    `fields` is shared by every query in the entry point, name -> field, so a
    value two loop bodies read travels in one field."""
    if fields is None:
        fields = {}
    if not outside:
        return []
    names = ', '.join(outside)
    if q.needs_intersection:
        raise rq.Unsupported(
            'Proceed loop body reads values defined outside it (%s) and becomes '
            'an intersection shader, which has no payload to carry them in' % names)
    if len(outside) > MAX_CARRY:
        raise rq.Unsupported(
            'Proceed loop body reads %d values defined outside it (%s); more '
            'than the %d the payload carries' % (len(outside), names, MAX_CARRY))
    defs = {i.result: (b, i) for b, i in fn.instrs() if i.result}
    dom = fn.dominators()
    tb = q.trace_block.label
    plan = []
    for k, name in enumerate(outside):
        if name not in defs:
            raise rq.Unsupported(
                'Proceed loop body reads %s, which no instruction defines; the '
                'any-hit shader is a separate invocation' % name)
        b, i = defs[name]
        before = (b.label == tb and i.index < q.trace.index) or \
                 (b.label != tb and b.label in dom.get(tb, set()))
        if not before:
            raise rq.Unsupported(
                'Proceed loop body reads %s, which is computed after the trace; '
                'the raygen cannot put it in the payload before TraceRay' % name)
        ty = _result_type(i)
        if ty not in _CARRY_TYPES:
            raise rq.Unsupported(
                'Proceed loop body reads %s, of type %s, from before it; the '
                'payload carries i32, float and i1' % (name, ty))
        if name not in fields:
            fields[name] = (ty, base + len(fields))
        plan.append((name, ty, fields[name][1]))
    if len(fields) > MAX_CARRY:
        raise rq.Unsupported(
            'the Proceed loops read %d values defined outside them between them; '
            'more than the %d the payload carries' % (len(fields), MAX_CARRY))
    return plan


def _payload_type(q):
    extra = ', i32' if q.others else ''
    for _, (ty, _f) in sorted(q.carry_fields.items(), key=lambda kv: kv[1][1]):
        extra += ', i32' if ty == 'i1' else ', ' + ty
    return PAYLOAD_TYPE[:-2] + extra + ' }'


def _payload_bytes(q):
    return PAYLOAD_BYTES + 4 * ((1 if q.others else 0) + len(q.carry_fields))


def _loops(q):
    """The queries whose loop body becomes the (shared) any-hit."""
    return [x for x in [q] + q.others if x.loop]


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
                '  %s = load i32, i32* %%%s.pl7, align 4\n'
                '  %s = icmp eq i32 %s, %d'
                % (tmp, q.pfx, instr.result, tmp, HIT_KIND_FRONT))
        elif instr.dxop == rq.COMMITTED_WORLD_TO_OBJECT:
            row = re.match(r'i32\s+(\d+)', instr.args[2].strip()).group(1)
            col = re.match(r'i8\s+(\d+)', instr.args[3].strip()).group(1)
            slot = int(row) * 4 + int(col)
            ptr = '%%rq.w%s' % instr.result[1:]
            edits[instr.index] = (
                '  %s = getelementptr inbounds [12 x float], [12 x float]* '
                '%%%s.pl8, i32 0, i32 %d\n'
                '  %s = load float, float* %s, align 4'
                % (ptr, q.pfx, slot, instr.result, ptr))
        elif instr.dxop == rq.COMMITTED_BARY:
            comp = re.match(r'i8\s+(\d+)', instr.args[2].strip()).group(1)
            tmp = '%%rq.bv%s' % instr.result[1:]
            edits[instr.index] = (
                '  %s = load <2 x float>, <2 x float>* %%%s.pl1, align 4\n'
                '  %s = extractelement <2 x float> %s, i32 %s'
                % (tmp, q.pfx, instr.result, tmp, comp))
        else:
            edits[instr.index] = '  %s = load %s, %s* %%%s.pl%d, align %d' % (
                instr.result, ty, ty, q.pfx, idx, align)


# --- generated text --------------------------------------------------------

def _add_types_and_globals(text, globals_, needs_record=False, binding=False,
                           sm66=False, payload_type=PAYLOAD_TYPE):
    decls = ['%s = type %s' % (PAYLOAD, payload_type),
             '%s = type { <2 x float> }' % ATTRS, '']
    if needs_record:
        # The raw buffer's element type, as DXC declares a ByteAddressBuffer.
        # ResRet is added only when the application's module lacks it.
        decls.insert(decls.index(''), '%s = type { i32 }' % RECORD_TYPE)
        if ('%s = type' % RESRET) not in text:
            decls.insert(decls.index(''), '%s = type { i32, i32, i32, i32, i32 }' % RESRET)
        if sm66 and '%dx.types.ResourceProperties = type' not in text:
            decls.insert(decls.index(''), '%dx.types.ResourceProperties = type { i32, i32 }')
        # At 6.6 the global holds a HANDLE and the record bitcasts it, as DXC's.
        decls.append('%s = external constant %s, align 4'
                     % (RECORD_GLOBAL, HANDLE_TYPE if sm66 else RECORD_TYPE))
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
                # also removes threadIdInGroup, which shares the prefix
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.threadId[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.groupId[^\n]*\n',
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.flattenedThreadIdInGroup[^\n]*\n',
                # The 6.6 handle creation is converted away, so its declare
                # goes too: an unused declare is itself a validation error.
                r'\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandleFromBinding[^\n]*\n']:
        text = re.sub(pat, '\n', text)

    # DispatchRaysIndex replaces threadId, so a shader that never read
    # SV_DispatchThreadID never calls it, and an unused declare is itself a
    # validation error. Every shader in this suite reads it, which is exactly
    # why this was unconditional; a real Unreal shader that gets its index from
    # somewhere else is what found it.
    new = []
    if any(i.dxop in _THREAD_OPS for _, i in q.fn.instrs()):
        new += ['', '; Function Attrs: nounwind readnone',
                'declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #RQNONE']
    new += ['', '; Function Attrs: nounwind',
           'declare void @dx.op.traceRay.%s(i32, %%dx.types.Handle, i32, i32, i32, '
           'i32, i32, float, float, float, float, float, float, float, float, %s*) #RQNW'
           % (PAYLOAD[1:], PAYLOAD),
           '', '; Function Attrs: nounwind readonly',
           'declare float @dx.op.rayTCurrent.f32(i32) #RQRO',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.instanceIndex.i32(i32) #RQNONE',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.primitiveIndex.i32(i32) #RQNONE',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.instanceID.i32(i32) #RQNONE',
           '', '; Function Attrs: nounwind readnone',
           'declare i32 @dx.op.hitKind.i32(i32) #RQNONE',
           ]
    # An UNUSED declare is itself a validation error, so these three are
    # emitted only when something actually calls them. The rest above are
    # always used, because the generated closest-hit reads them every time.
    used_ops = set()
    for x in [q] + q.others:
        used_ops |= {i.dxop for _, i in x.candidate_ops} | {i.dxop for _, i in x.committed_ops}
    conditional = [
        ({rq.CANDIDATE_WORLD_TO_OBJECT, rq.COMMITTED_WORLD_TO_OBJECT},
         'declare float @dx.op.worldToObject.f32(i32, i32, i8) #RQNONE'),
        ({rq.CANDIDATE_OBJECT_RAY_ORIGIN},
         'declare float @dx.op.objectRayOrigin.f32(i32, i8) #RQNONE'),
        ({rq.CANDIDATE_OBJECT_RAY_DIRECTION},
         'declare float @dx.op.objectRayDirection.f32(i32, i8) #RQNONE'),
    ]
    # Only reached from a generated hit shader. A raygen read of RayFlags
    # became a constant, so a shader reading it only there declares nothing,
    # and an unused declare is itself a validation error.
    if any(x.rayflags_in_loop for x in [q] + q.others):
        new += ['', '; Function Attrs: nounwind readnone',
                'declare i32 @dx.op.rayFlags.i32(i32) #RQNONE']
    for ops, decl in conditional:
        if used_ops & ops:
            new += ['', '; Function Attrs: nounwind readnone', decl]

    # The record read. Both are nounwind readonly, copied from DXC output.
    #
    # rawBufferLoad may ALREADY be declared, because the application's own
    # shader may read a raw buffer of its own; declaring it twice is as much an
    # error as declaring it unused.
    if q.needs_record_constants and q.record_sm66:
        # The binding form declares this overload itself, further down.
        if not binding and '@dx.op.createHandleForLib.dx.types.Handle(' not in text:
            new += ['', '; Function Attrs: nounwind readonly',
                    'declare %s @dx.op.createHandleForLib.dx.types.Handle'
                    '(i32, %s) #RQRO' % (HANDLE_TYPE, HANDLE_TYPE)]
        if '@dx.op.annotateHandle(' not in text:
            new += ['', '; Function Attrs: nounwind readnone',
                    'declare %s @dx.op.annotateHandle(i32, %s, '
                    '%%dx.types.ResourceProperties) #RQNONE' % (HANDLE_TYPE, HANDLE_TYPE)]
    elif q.needs_record_constants:
        new += ['', '; Function Attrs: nounwind readonly',
                'declare %%dx.types.Handle @%s(i32, %s) #RQRO'
                % (RECORD_HANDLE_FN, RECORD_TYPE)]
    if q.needs_record_constants:
        if '@dx.op.rawBufferLoad.i32(' not in text:
            new += ['', '; Function Attrs: nounwind readonly',
                    'declare %s @dx.op.rawBufferLoad.i32'
                    '(i32, %%dx.types.Handle, i32, i32, i8, i32) #RQRO' % RESRET]
    if q.needs_intersection:
        new += ['', '; Function Attrs: nounwind',
                'declare i1 @dx.op.reportHit.%s(i32, float, i32, %s*) #RQNW'
                % (ATTRS[1:], ATTRS)]
    # Unconditional now: the generated any-hit may or may not exist, but
    # AnyHitNull always does and it is nothing but an IgnoreHit.
    new += ['', '; Function Attrs: noreturn nounwind',
            'declare void @dx.op.ignoreHit(i32) #RQNR']
    if binding:
        # One overload serves every resource in the 6.6 form, because the
        # global is a handle whatever the resource is.
        new += ['', '; Function Attrs: nounwind readonly',
                'declare %s @dx.op.createHandleForLib.dx.types.Handle'
                '(i32, %s) #RQRO' % (HANDLE_TYPE, HANDLE_TYPE)]
    else:
        seen = set()
        for sym, gty, elem, _ in globals_.values():
            fnname, _ = _handle_fn(elem)
            if fnname in seen:
                continue
            seen.add(fnname)
            new += ['', '; Function Attrs: nounwind readonly',
                    'declare %%dx.types.Handle @%s(i32, %s) #RQRO' % (fnname, elem)]

    anchor = text.index('\nattributes #0 =')
    text = text[:anchor] + '\n' + '\n'.join(new) + text[anchor:]

    return text


def _closesthit(name, status, q):
    """The generated closest-hit, writing `status` as the committed kind.

    A both-kinds query needs TWO of these. A triangle hit reports
    COMMITTED_TRIANGLE_HIT and a procedural one
    COMMITTED_PROCEDURAL_PRIMITIVE_HIT, and one shader cannot say both,
    because it does not know which hit group resolved to it.
    """
    ch = ['define void @{ch}({pl}* noalias nocapture %p, {at}* nocapture readonly %attr) #RQNW {{'
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
    # stored whenever either is read: one load answers both, so splitting
    # them would cost a branch and save nothing.
    if q.needs_record_constants:
        ch += _record_read('g', 0, '%rq.geo', q.record_sm66)
        ch.append('  %%rq.gp = getelementptr inbounds %s, %s* %%p, i32 0, i32 5'
                  % (PAYLOAD, PAYLOAD))
        ch.append('  store i32 %rq.geo, i32* %rq.gp, align 4')
        ch.append('  %%rq.con = extractvalue %s %%rq.cbrg, 1' % RESRET)
        ch.append('  %%rq.cp = getelementptr inbounds %s, %s* %%p, i32 0, i32 10'
                  % (PAYLOAD, PAYLOAD))
        ch.append('  store i32 %rq.con, i32* %rq.cp, align 4')

    # Twelve fetches and twelve stores, so they are emitted ONLY when the
    # shader actually reads the matrix. The payload field exists either way,
    # because a layout that changes shape is a layout that gets an offset
    # wrong; it is the per-invocation work that is worth avoiding.
    if any(i.dxop == rq.COMMITTED_WORLD_TO_OBJECT
           for x in [q] + q.others for _, i in x.committed_ops):
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
    elif _loops(q):
        fns.append(_anyhit(module, q, exports, table, globals_, binds))

    if q.needs_both:
        # And two closest-hits, because the committed status differs and a
        # closest-hit cannot tell which hit group resolved to it.
        fns.append(_closesthit(exports['closesthit'], 1, q))
        fns.append(_closesthit(exports['closesthitproc'], 2, q))
    else:
        fns.append(_closesthit(exports['closesthit'],
                               2 if q.needs_intersection else 1, q))
    fns.append('''define void @{ms}({pl}* noalias nocapture %p) #RQNW {{
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
    fns.append('''define void @{ah}({pl}* noalias nocapture %p, {at}* nocapture readnone %attr) #RQNR {{
  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()
  unreachable
}}'''.format(ah=exports['anyhitnull'], pl=PAYLOAD, at=ATTRS))

    # And reporting nothing is how a PROCEDURAL hit group produces no hit: an
    # intersection shader that returns has found nothing.
    fns.append('''define void @{is}() #RQNW {{
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
    out = ['define void @%s() #RQNW {' % exports['intersection'],
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
        elif i.dxop in _THREAD_OPS:
            out.append(_thread_index_text(i, module))
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
                out += _record_read(i.result[1:], RECORD_WORD[i.dxop], i.result,
                                    q.record_sm66)
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
    loops = _loops(q)
    multi = bool(q.others)

    subst = {}
    for x in loops:
        header, latch, body = x.loop
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
                    # CandidateType has just folded away. Dead, but it still
                    # has to be a value.
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
    aborts = any(x.aborts for x in loops)

    out = ['define void @%s(%s* noalias nocapture %%p, %s* nocapture readonly %%attr) #RQNW {'
           % (exports['anyhit'], PAYLOAD, ATTRS),
           '  %rq.ap = getelementptr inbounds {at}, {at}* %attr, i32 0, i32 0'.format(at=ATTRS),
           '  %rq.ab = load <2 x float>, <2 x float>* %rq.ap, align 4']
    for name, (ty, field) in sorted(q.carry_fields.items(), key=lambda kv: kv[1][1]):
        out.append('  %%rq.pac%d = getelementptr inbounds %s, %s* %%p, i32 0, i32 %d'
                   % (field, PAYLOAD, PAYLOAD, field))
        if ty == 'i1':
            out.append('  %%rq.pav%d = load i32, i32* %%rq.pac%d, align 4' % (field, field))
            out.append('  %s = icmp ne i32 %%rq.pav%d, 0' % (name, field))
        else:
            out.append('  %s = load %s, %s* %%rq.pac%d, align 4' % (name, ty, ty, field))

    # Recreate every resource handle the body uses, under the SAME SSA name it
    # had in the raygen, so the transplanted instructions need no rewriting.
    used, inBody = set(), set()
    for x in loops:
        header, latch, body = x.loop
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
        elif i.dxop in _THREAD_OPS:
            out.append(_thread_index_text(i, module))
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

    if multi:
        # Which query's trace this is, set by the raygen before TraceRay.
        out.append('  %%rq.pq = getelementptr inbounds %s, %s* %%p, i32 0, i32 %d'
                   % (PAYLOAD, PAYLOAD, QID_FIELD))
        out.append('  %rq.qid = load i32, i32* %rq.pq, align 4')
        for n, x in enumerate(loops[1:], 1):
            nxt = '%%rq.qn%d' % n
            out.append('  %%rq.qc%d = icmp eq i32 %%rq.qid, %d' % (n, x.qid))
            out.append('  br i1 %%rq.qc%d, label %s, label %s' % (n, x.loop[0], nxt))
            out.append('')
            out.append('%s:' % nxt[1:])
        out.append('  br label %s' % loops[0].loop[0])

    for x in loops:
        header, latch, body = x.loop
        commit_blocks = {b.label for b, _ in x.commits}
        order = [header] + sorted(l for l in body if l not in (header, latch))
        _anyhit_body(fn, x, order, header, latch, body, commit_blocks, subst,
                     multi, out)

    out += ['', 'rq.accept:', '  ret void',
            '', 'rq.reject:',
            '  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()',
            '  unreachable', '}']
    return '\n'.join(out)


def _anyhit_body(fn, q, order, header, latch, body, commit_blocks, subst, multi, out):
    """One query's loop body, as blocks of the any-hit."""
    for label in order:
        blk = fn.block(label)
        if multi or label != header:
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
                out += _record_read(i.result[1:], RECORD_WORD[i.dxop], i.result,
                                    q.record_sm66)
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

    # The record SRV, added to whatever the application already had.
    #
    # Shape copied from DXC, not invented: an SRV record is
    # {id, global, name, space, lowerBound, rangeSize, kind, sampleCount,
    # extra}, and kind 11 is a raw buffer. space 1 keeps it clear of anything
    # the application's own global root signature binds, which the shim must
    # not disturb. See phase5/cases/reference/lib_localsrv_ref.hlsl.
    if q.needs_record_constants:
        groups = [g.strip() for g in md[int(res)].split(',')]
        while len(groups) < 4:
            groups.append('null')
        have = []
        if groups[0] != 'null':
            have = [x.strip() for x in md[int(groups[0][1:])].split(',')]
        gref = ('%s* bitcast (%s* %s to %s*)'
                % (RECORD_TYPE, HANDLE_TYPE, RECORD_GLOBAL, RECORD_TYPE)
                if q.record_sm66 else '%s* %s' % (RECORD_TYPE, RECORD_GLOBAL))
        rec = node('i32 %d, %s, !"rq_record", i32 1, i32 0, i32 1, i32 11, i32 0, null'
                   % (len(have), gref))
        groups[0] = '!%d' % node(', '.join(have + ['!%d' % rec]))
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
    elif _loops(q):
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
    flags = node('i32 0, i64 %d'
                 % ((_module_flags(text, md) & ~RT11_SHADER_FLAG)
                    | (RAW_BUFFER_FLAG if q.needs_record_constants else 0)))
    eps = [node('null, !"", null, !%s, !%d' % (res, flags))]

    def entry(name, sig, kind, payload, attrs):
        # Which property tags appear depends on the shader kind, exactly as
        # DXC emits them: a raygen carries neither size, a miss carries the
        # payload size only, and hit shaders carry both.
        props = ['i32 8', 'i32 %d' % kind]
        if payload:
            props += ['i32 6', 'i32 %d' % _payload_bytes(q)]
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
    elif _loops(q):
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
