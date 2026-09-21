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
COMMIT_NON_OPAQUE = 182
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

# Opcode -> short name, for messages. Membership in this table is what makes
# an opcode supported; the numbers NOT here are deliberately absent because
# this project has not observed them and will not guess.
KNOWN = {
    ALLOCATE: 'AllocateRayQuery',
    TRACE_INLINE: 'TraceRayInline',
    PROCEED: 'Proceed',
    COMMIT_NON_OPAQUE: 'CommitNonOpaqueTriangleHit',
    COMMITTED_STATUS: 'CommittedStatus',
    CANDIDATE_TYPE: 'CandidateType',
    CANDIDATE_BARY: 'CandidateTriangleBarycentrics',
    COMMITTED_BARY: 'CommittedTriangleBarycentrics',
    COMMITTED_RAY_T: 'CommittedRayT',
    COMMITTED_INSTANCE_INDEX: 'CommittedInstanceIndex',
    COMMITTED_GEOMETRY_INDEX: 'CommittedGeometryIndex',
    COMMITTED_PRIMITIVE_INDEX: 'CommittedPrimitiveIndex',
}

# Everything the generated closest-hit can put in the payload. Used by both the
# analysis, to decide what is a committed read, and the lowering, to lay the
# payload out.
COMMITTED_OPS = (COMMITTED_STATUS, COMMITTED_BARY, COMMITTED_RAY_T,
                 COMMITTED_INSTANCE_INDEX, COMMITTED_GEOMETRY_INDEX,
                 COMMITTED_PRIMITIVE_INDEX)

# CommittedGeometryIndex is recognised so the refusal can explain itself, but
# it has NO lowering on this hardware. Its DXR 1.0 equivalent, GeometryIndex()
# in a hit shader, is itself a Tier 1.1 feature: measured, a library using it
# sets shader flag 0x2000000 and CreateStateObject on the GTX 1070 fails with
# E_INVALIDARG. The known route would be to encode the geometry index in the
# shader table with one hit group record per geometry, which means the shim
# rebuilding the application's SBT. Not attempted.
NO_LOWERING = {
    COMMITTED_GEOMETRY_INDEX:
        'CommittedGeometryIndex has no lowering on Tier 1.0. Its DXR 1.0 '
        'equivalent, GeometryIndex() in a hit shader, is itself a Tier 1.1 '
        'feature and CreateStateObject rejects it on this hardware. Encoding '
        'the index in the shader table would work but is not implemented.',
}

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
        self.candidate_ops = []
        self.committed_ops = []
        self.loop = None            # (header, latch, body) or None

    # --- derived ---------------------------------------------------------

    @property
    def dyn_flags(self):
        """The flags passed to TraceRayInline, which OR with the template's."""
        return _imm(self.trace.args[3])

    @property
    def ray_flags(self):
        return (self.const_flags or 0) | (self.dyn_flags or 0)

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

    def pattern(self):
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
        elif op in (CANDIDATE_TYPE, CANDIDATE_BARY):
            q.candidate_ops.append((block, instr))
        elif op in COMMITTED_OPS:
            if op in NO_LOWERING:
                raise Unsupported(NO_LOWERING[op])
            q.committed_ops.append((block, instr))

    if q.trace is None:
        raise Unsupported('query is allocated but never traced')


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
