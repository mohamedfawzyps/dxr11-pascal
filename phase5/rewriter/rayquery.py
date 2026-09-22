#!/usr/bin/env python3
"""Find and classify RayQuery use in a DXIL module, structurally.

The hand lowerings in phase5/hand key off exact instruction text, which works
for a fixed input and is useless for a real one. This finds the same sites the
way the recon says the IR supports:

  - follow the query handle, which is a plain i32 SSA value, so ownership is
    one def-use hop and nothing needs alias analysis
  - discriminate on the opcode IMMEDIATE, never the callee name, because DXC
    reuses one LLVM function for several opcodes
  - recognise the rotated loop through real dominator analysis, because the
    source's single `while (q.Proceed())` becomes two Proceed calls with the
    guard peeled into the preheader

Opcodes are a WHITELIST. Every rayQuery opcode this project has actually
verified against DXC output is listed; anything else makes analysis fail
loudly rather than silently lower something it does not understand. The brief
is explicit about that preference, and a wrong render is far worse than a
refusal.
"""

import re

# Verified against DXC 1.10.2605.37 output, see docs/phase5-dxil-recon.md.
ALLOCATE = 178
TRACE_INLINE = 179
PROCEED = 180
ABORT = 181
COMMIT_NON_OPAQUE = 182
COMMIT_PROCEDURAL = 183
CANDIDATE_PROC_NON_OPAQUE = 190
COMMITTED_STATUS = 184
CANDIDATE_TYPE = 185
CANDIDATE_BARY = 193
COMMITTED_BARY = 194
COMMITTED_RAY_T = 200
# Read off DXC output the same way as the rest. All three share the
# StateScalar.i32 function with 184 and 185, which is why operand 0 is the only
# safe discriminator.
COMMITTED_INSTANCE_INDEX = 207
COMMITTED_GEOMETRY_INDEX = 209
COMMITTED_PRIMITIVE_INDEX = 210

# The accessors a real engine uses, added after surveying Unreal. All read off
# DXC output, and every DXR 1.0 target was compiled and its SFI0 checked first,
# because GeometryIndex proved an intrinsic can look ordinary and be secretly
# Tier 1.1.
CANDIDATE_WORLD_TO_OBJECT = 187
CANDIDATE_FRONT_FACE = 191
COMMITTED_FRONT_FACE = 192
COMMITTED_WORLD_TO_OBJECT = 189
CANDIDATE_RAY_T = 199
CANDIDATE_INSTANCE_INDEX = 201
CANDIDATE_INSTANCE_ID = 202
CANDIDATE_PRIMITIVE_INDEX = 204
CANDIDATE_OBJECT_RAY_ORIGIN = 205
CANDIDATE_OBJECT_RAY_DIRECTION = 206
COMMITTED_INSTANCE_ID = 208

# The four the SHADER TABLE answers, because nothing in the shader can.
#
# GeometryIndex() in a DXR 1.0 hit shader is itself Tier 1.1, and HLSL exposes
# no hit-shader intrinsic for the contribution at all. Both were written off as
# impossible for reasons that only hold while the APPLICATION owns the table.
# The shim builds it, so each record carries these two numbers as local root
# signature constants and the hit shader reads them back. Measured at
# SFI0 = 0x0: phase5/cases/reference/lib_localroot_ref.hlsl.
#
# Numbers read off DXC, not inferred from the gaps around them:
# phase5/cases/reference/rq_opcodes_ref.hlsl.
CANDIDATE_GEOMETRY_INDEX = 203
CANDIDATE_INSTANCE_CONTRIB = 214
COMMITTED_INSTANCE_CONTRIB = 215

# Query-wide, neither candidate nor committed: the flags the query is tracing
# with. 59 of Unreal's shaders refused on this once GeometryIndex stopped
# blocking them first, which made it the largest single remaining bucket.
RAY_FLAGS = 195

# Opcode -> short name, for messages. Membership in this table is what makes
# an opcode supported; the numbers NOT here are deliberately absent because
# this project has not observed them and will not guess.
KNOWN = {
    ALLOCATE: 'AllocateRayQuery',
    TRACE_INLINE: 'TraceRayInline',
    PROCEED: 'Proceed',
    ABORT: 'Abort',
    COMMIT_NON_OPAQUE: 'CommitNonOpaqueTriangleHit',
    COMMIT_PROCEDURAL: 'CommitProceduralPrimitiveHit',
    CANDIDATE_PROC_NON_OPAQUE: 'CandidateProceduralPrimitiveNonOpaque',
    COMMITTED_STATUS: 'CommittedStatus',
    CANDIDATE_TYPE: 'CandidateType',
    CANDIDATE_BARY: 'CandidateTriangleBarycentrics',
    COMMITTED_BARY: 'CommittedTriangleBarycentrics',
    COMMITTED_RAY_T: 'CommittedRayT',
    COMMITTED_INSTANCE_INDEX: 'CommittedInstanceIndex',
    COMMITTED_GEOMETRY_INDEX: 'CommittedGeometryIndex',
    COMMITTED_PRIMITIVE_INDEX: 'CommittedPrimitiveIndex',
    CANDIDATE_WORLD_TO_OBJECT: 'CandidateWorldToObject',
    CANDIDATE_FRONT_FACE: 'CandidateTriangleFrontFace',
    COMMITTED_FRONT_FACE: 'CommittedTriangleFrontFace',
    COMMITTED_WORLD_TO_OBJECT: 'CommittedWorldToObject',
    CANDIDATE_RAY_T: 'CandidateTriangleRayT',
    CANDIDATE_INSTANCE_INDEX: 'CandidateInstanceIndex',
    CANDIDATE_INSTANCE_ID: 'CandidateInstanceID',
    CANDIDATE_PRIMITIVE_INDEX: 'CandidatePrimitiveIndex',
    CANDIDATE_OBJECT_RAY_ORIGIN: 'CandidateObjectRayOrigin',
    CANDIDATE_OBJECT_RAY_DIRECTION: 'CandidateObjectRayDirection',
    COMMITTED_INSTANCE_ID: 'CommittedInstanceID',
    RAY_FLAGS: 'RayFlags',
    CANDIDATE_GEOMETRY_INDEX: 'CandidateGeometryIndex',
    CANDIDATE_INSTANCE_CONTRIB: 'CandidateInstanceContributionToHitGroupIndex',
    COMMITTED_INSTANCE_CONTRIB: 'CommittedInstanceContributionToHitGroupIndex',
}

# The two read in the any-hit or intersection shader straight out of the
# record, and the two that travel home in the payload from the closest-hit.
RECORD_CANDIDATE_OPS = (CANDIDATE_GEOMETRY_INDEX, CANDIDATE_INSTANCE_CONTRIB)
RECORD_COMMITTED_OPS = (COMMITTED_GEOMETRY_INDEX, COMMITTED_INSTANCE_CONTRIB)
RECORD_OPS = RECORD_CANDIDATE_OPS + RECORD_COMMITTED_OPS

# Read in the any-hit shader, where they need no payload at all: the candidate
# under test IS what a DXR 1.0 hit-shader intrinsic reports.
CANDIDATE_OPS = (CANDIDATE_PROC_NON_OPAQUE,
                 CANDIDATE_TYPE, CANDIDATE_BARY, CANDIDATE_WORLD_TO_OBJECT,
                 CANDIDATE_FRONT_FACE, CANDIDATE_RAY_T, CANDIDATE_INSTANCE_INDEX,
                 CANDIDATE_INSTANCE_ID, CANDIDATE_PRIMITIVE_INDEX,
                 CANDIDATE_OBJECT_RAY_ORIGIN, CANDIDATE_OBJECT_RAY_DIRECTION,
                 CANDIDATE_GEOMETRY_INDEX, CANDIDATE_INSTANCE_CONTRIB,
                 RAY_FLAGS)

# Everything the generated closest-hit can put in the payload. Used by both the
# analysis, to decide what is a committed read, and the lowering, to lay the
# payload out.
COMMITTED_OPS = (COMMITTED_STATUS, COMMITTED_BARY, COMMITTED_RAY_T,
                 COMMITTED_INSTANCE_INDEX, COMMITTED_GEOMETRY_INDEX,
                 COMMITTED_PRIMITIVE_INDEX, COMMITTED_INSTANCE_ID,
                 COMMITTED_FRONT_FACE, COMMITTED_WORLD_TO_OBJECT,
                 COMMITTED_INSTANCE_CONTRIB)

# Empty, and that is the interesting part.
#
# CommittedGeometryIndex lived here for most of this project's life, because
# GeometryIndex() in a DXR 1.0 hit shader is ITSELF Tier 1.1: a library using
# it sets shader flag 0x2000000 and CreateStateObject on the GTX 1070 returns
# E_INVALIDARG. That measurement is still true. What changed is that the answer
# no longer has to come from an intrinsic: the shim builds the shader table, so
# it puts the geometry index in the record.
#
# A real Unreal 5.8 run refused 120 shaders on the four record opcodes, more
# than everything else together, which is what made it worth doing.
#
# Kept as a table rather than deleted, because the next opcode with no lowering
# will want somewhere to say so.
NO_LOWERING = {}

RAY_FLAG = [
    (0x001, 'FORCE_OPAQUE'),
    (0x002, 'FORCE_NON_OPAQUE'),
    (0x004, 'ACCEPT_FIRST_HIT_AND_END_SEARCH'),
    (0x008, 'SKIP_CLOSEST_HIT_SHADER'),
    (0x010, 'CULL_BACK_FACING_TRIANGLES'),
    (0x020, 'CULL_FRONT_FACING_TRIANGLES'),
    (0x040, 'CULL_OPAQUE'),
    (0x080, 'CULL_NON_OPAQUE'),
    (0x100, 'SKIP_TRIANGLES'),
    (0x200, 'SKIP_PROCEDURAL_PRIMITIVES'),
]


def flag_names(value):
    names = [n for bit, n in RAY_FLAG if value & bit]
    return '|'.join(names) if names else 'NONE'


class Unsupported(Exception):
    """This shader has no valid lowering, or one this project cannot do yet.

    Raised rather than returned, because the only correct response is to stop.
    A shim that claims Tier 1.1 and then renders the wrong thing is worse than
    one that refuses."""


class Query(object):
    """One RayQuery object and everything reachable from its handle."""

    def __init__(self, fn, alloc_block, alloc):
        self.fn = fn
        self.handle = alloc.result
        self.alloc = alloc
        self.alloc_block = alloc_block
        self.const_flags = _imm(alloc.args[1])
        self.trace = None
        self.trace_block = None
        self.proceeds = []          # [(block, instr)]
        self.commits = []
        self.aborts = []
        self.proc_commits = []
        self.candidate_ops = []
        self.committed_ops = []
        self.loop = None            # (header, latch, body) or None
        # Does any accessor need the answer that only the shader TABLE has?
        # Set by the collect pass; drives the local root signature and the
        # wider hit records, both of which cost something and are therefore
        # only added for a shader that asks.
        self.needs_record_constants = False
        # Set by the lowering: is RayFlags read INSIDE the Proceed loop?
        # Only then does a generated hit shader call dx.op.rayFlags, and
        # only then may it be declared, since an unused declare is itself a
        # validation error.
        self.rayflags_in_loop = False

    # --- derived ---------------------------------------------------------

    @property
    def dyn_flags(self):
        """The flags passed to TraceRayInline, which OR with the template's."""
        return _imm(self.trace.args[3])

    @property
    def ray_flags(self):
        """The flags KNOWN AT COMPILE TIME. Used for classification only.

        When TraceRayInline is given a runtime value, dyn_flags is None and
        this is just the template's flags. That is the honest answer for
        deciding whether traversal is provably fixed-function: it is not, so a
        query with no Proceed loop and no FORCE_OPAQUE template is still
        refused. What must NOT use this is the traceRay call itself, which
        needs flags_operand."""
        return (self.const_flags or 0) | (self.dyn_flags or 0)

    @property
    def flags_operand(self):
        """The RayFlags operand text for traceRay, and any setup it needs.

        (setup_lines, operand_text). Static flags are a literal; a runtime
        value is OR'd with the template's flags, because RayQuery<FLAGS> means
        both apply and TraceRay takes only one operand."""
        if self.dyn_flags is not None:
            return [], 'i32 %d' % self.ray_flags
        from dxil import operand_name
        dyn = operand_name(self.trace.args[3])
        return (['  %%rq.flags = or i32 %s, %d' % (dyn, self.const_flags or 0)],
                'i32 %rq.flags')

    @property
    def as_handle(self):
        from dxil import operand_name
        return operand_name(self.trace.args[2])

    @property
    def ray_args(self):
        """mask, origin xyz, tmin, direction xyz, tmax, as operand text."""
        a = self.trace.args
        return {'mask': a[4], 'origin': a[5:8], 'tmin': a[8],
                'direction': a[9:12], 'tmax': a[12]}

    @property
    def needs_anyhit(self):
        """A Proceed loop means candidates are being inspected in the shader,
        which is exactly what an any-hit shader does."""
        return self.loop is not None

    @property
    def needs_intersection(self):
        """A procedural commit means the loop body is an INTERSECTION shader."""
        return bool(self.proc_commits)

    @property
    def needs_both(self):
        """Commits of BOTH kinds, so the loop body becomes TWO shaders.

        A hit group is either triangles or procedural, never both, so this
        needs two of them, and two closest-hits as well, because one writes
        committed status 1 and the other 2. The scene side of it is the shim's
        problem: which record a geometry resolves to is chosen by
        InstanceContributionToHitGroupIndex, which the APPLICATION set. That is
        handled by the typed shader table, and is refused only when the
        application routed both kinds to the SAME record.
        """
        return bool(self.commits) and bool(self.proc_commits)

    def pattern(self):
        if self.needs_both:
            return 5, 'both triangle and procedural commits (two hit groups)'
        if self.needs_intersection:
            return 4, 'procedural primitives (generated intersection shader)'
        if self.needs_anyhit:
            return 3, 'alpha-tested closest hit (generated any-hit shader)'
        if self.ray_flags & 0x004:
            return 2, 'shadow / visibility (miss shader only)'
        if self.ray_flags & 0x001:
            return 1, 'opaque closest hit (no any-hit shader)'
        # No loop and not forced opaque: traversal would have yielded
        # candidates the shader never inspected, so the source never committed
        # them. Refuse rather than guess what was meant.
        raise Unsupported(
            'query has no Proceed loop but is not FORCE_OPAQUE '
            '(flags %s); no lowering is defined for this' % flag_names(self.ray_flags))


def _imm(arg):
    m = re.match(r'^i32\s+(-?\d+)$', arg.strip())
    return int(m.group(1)) if m else None


def analyze(module, fn_name=None):
    """Return the single Query in the module, or raise Unsupported."""
    _reject_module(module)

    fn = module.function(fn_name) if fn_name else None
    if fn is None:
        cands = [f for f in module.functions if f.find(ALLOCATE)]
        if not cands:
            raise Unsupported('no dx.op.allocateRayQuery in this module')
        if len(cands) > 1:
            raise Unsupported('RayQuery used in %d functions; only one entry '
                              'point is supported' % len(cands))
        fn = cands[0]

    allocs = fn.find(ALLOCATE)
    if len(allocs) != 1:
        # TraceRay has one payload and one in-flight trace, so concurrent
        # queries have no lowering. Named in the brief's table.
        raise Unsupported('%d concurrent RayQuery objects in %s; TraceRay has '
                          'one payload and one in-flight trace'
                          % (len(allocs), fn.name))

    q = Query(fn, allocs[0][0], allocs[0][1])
    _collect(fn, q)
    _find_loop(fn, q)
    _reject_function(fn, q)
    # Classify here rather than on demand, so that a shape with no defined
    # lowering is refused by analysis itself. Leaving it lazy meant a caller
    # that never asked for the pattern got a Query back for a shader that
    # cannot be lowered at all.
    q.pattern_num, q.pattern_desc = q.pattern()
    return q


def _collect(fn, q):
    """Walk every dx.op call that reads the query handle."""
    for block, instr in fn.instrs():
        op = instr.dxop
        if op is None:
            continue
        # Only rayQuery ops take the handle; anything else is ordinary code.
        if q.handle not in instr.uses():
            if op in KNOWN and op != ALLOCATE:
                raise Unsupported('%s at "%s" does not use the query handle'
                                  % (KNOWN[op], instr.body[:60]))
            continue
        if op not in KNOWN:
            raise Unsupported(
                'unrecognised rayQuery opcode %d at "%s". This project has '
                'only verified %s. Refusing rather than guessing.'
                % (op, instr.body[:60], ', '.join(str(k) for k in sorted(KNOWN))))
        if op == TRACE_INLINE:
            if q.trace is not None:
                raise Unsupported('more than one TraceRayInline on one query')
            q.trace, q.trace_block = instr, block
        elif op == PROCEED:
            q.proceeds.append((block, instr))
        elif op == COMMIT_NON_OPAQUE:
            q.commits.append((block, instr))
        elif op == ABORT:
            q.aborts.append((block, instr))
        elif op == COMMIT_PROCEDURAL:
            q.proc_commits.append((block, instr))
        elif op in CANDIDATE_OPS:
            q.candidate_ops.append((block, instr))
            if op in RECORD_OPS:
                q.needs_record_constants = True
        elif op in COMMITTED_OPS:
            if op in NO_LOWERING:
                raise Unsupported(NO_LOWERING[op])
            q.committed_ops.append((block, instr))
            # The state object then needs a local root signature and every hit
            # record two more words. Both cost something, so they are added
            # only for a shader that actually asks.
            if op in RECORD_OPS:
                q.needs_record_constants = True

    if q.trace is None:
        raise Unsupported('query is allocated but never traced')

    # Ray flags computed at runtime are FINE. dx.op.traceRay takes its
    # RayFlags as an ordinary i32 operand, not an immediate: measured, see
    # phase5/cases/reference/lib_dynflags_ref.hlsl, where DXC emits `i32 %7`
    # and the container signs.
    #
    # They were refused for one version, because `(dyn_flags or 0)` had been
    # silently folding an unknown value to 0 and tracing with the wrong flags.
    # Refusing was the right answer to THAT; passing the value through is the
    # right answer to the question. Unreal computes its ray flags at runtime in
    # 118 shaders, so folding was never going to be enough.


def _find_loop(fn, q):
    """Locate the rotated Proceed loop, if there is one.

    The source's `while (q.Proceed())` becomes two Proceed calls: a guard in
    the preheader and the real test in the latch. So look for a natural loop
    whose latch contains a Proceed on this handle."""
    proceed_blocks = {b.label for b, _ in q.proceeds}
    found = []
    for header, latch, body in fn.natural_loops():
        if latch in proceed_blocks:
            found.append((header, latch, body))
    if len(found) > 1:
        raise Unsupported('more than one Proceed loop on one query')
    if found:
        q.loop = found[0]
        return

    # No loop. One bare Proceed is the FORCE_OPAQUE shape, where traversal is
    # entirely fixed function and Proceed never yields.
    if len(q.proceeds) > 1:
        raise Unsupported('%d Proceed calls but no loop; unrecognised shape'
                          % len(q.proceeds))


def _reject_module(module):
    """Cases the brief says have no valid lowering, checked before anything."""
    m = re.search(r'!dx\.shaderModel\s*=\s*!\{!(\d+)\}', module.text)
    if m:
        node = re.search(r'^!%s = !\{!"(\w+)", i32 (\d+), i32 (\d+)\}'
                         % m.group(1), module.text, re.M)
        if node and node.group(1) != 'cs':
            raise Unsupported(
                'RayQuery in a "%s" shader. DispatchRays only launches raygen, '
                'so there is no promotion path from any other stage.'
                % node.group(1))

    if re.search(r'addrspace\(3\)', module.text):
        raise Unsupported('shader uses groupshared memory; a raygen shader has '
                          'no thread group')


def _reject_function(fn, q):
    """Checks that need the query, so they run after collection."""
    for _, instr in fn.instrs():
        op = instr.dxop
        if op is None:
            continue
        if op == 80:                      # dx.op.barrier
            raise Unsupported('shader uses a group barrier; a raygen shader '
                              'has no thread group')
        if instr.callee and '.wave' in instr.callee.lower():
            raise Unsupported('shader uses wave intrinsics around the query; '
                              'promotion to raygen changes lane occupancy')

    for block, instr in q.aborts:
        if not q.loop or block.label not in q.loop[2]:
            raise Unsupported(
                'Abort() outside the Proceed loop; traversal can only be '
                'stopped from the generated any-hit shader')

    if q.loop:
        _, _, body = q.loop
        for label in body:
            blk = fn.block(label)
            for instr in blk.instrs:
                if instr.dxop in COMMITTED_OPS:
                    raise Unsupported(
                        'committed state is read inside the Proceed loop; the '
                        'any-hit shader is a separate invocation and cannot '
                        'see it')
