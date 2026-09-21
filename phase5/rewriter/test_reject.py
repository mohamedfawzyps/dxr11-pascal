#!/usr/bin/env python3
"""Prove the analysis refuses what it should refuse.

The brief is explicit: detect the cases with no valid lowering and fail loudly
rather than produce wrong output. An analysis that never says no is worth as
little as one that never finds anything, so each refusal is provoked here by
mutating a known-good input.

    python phase5/rewriter/test_reject.py
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'hand'))

from dxil import Module
from llnorm import normalize
import rayquery

OPAQUE = os.path.join('phase5', 'dxil', 'rayquery_opaque.ll')
ALPHA = os.path.join('phase5', 'dxil', 'rayquery_alpha.ll')
INDEP = os.path.join('phase5', 'cases', 'rayquery_indep.ll')
IDS = os.path.join('phase5', 'cases', 'rayquery_ids.ll')


def load(path):
    return normalize(io.open(path, encoding='utf-8').read())


def expect_ok(name, text):
    try:
        q = rayquery.analyze(Module(text))
        print('  ok       %-34s pattern %d' % (name, q.pattern()[0]))
        return True
    except rayquery.Unsupported as e:
        print('  FAILED   %-34s unexpectedly refused: %s' % (name, e))
        return False


def expect_reject(name, text, must_mention):
    try:
        rayquery.analyze(Module(text))
        print('  FAILED   %-34s was ACCEPTED, should have been refused' % name)
        return False
    except rayquery.Unsupported as e:
        if must_mention.lower() not in str(e).lower():
            print('  FAILED   %-34s refused for the wrong reason: %s'
                  % (name, e))
            return False
        print('  refused  %-34s %s' % (name, str(e).split('.')[0][:52]))
        return True


def expect_pattern(name, text, want):
    """Accepted AND classified as `want`.

    Plain acceptance is too weak for a case that used to be refused: falling
    back to a NEIGHBOURING pattern would still be accepted and would still be
    wrong.
    """
    try:
        q = rayquery.analyze(Module(text))
        got = q.pattern()[0]
        if got != want:
            print('  FAILED   %-34s pattern %d, expected %d' % (name, got, want))
            return False
        print('  ok       %-34s pattern %d' % (name, got))
        return True
    except rayquery.Unsupported as e:
        print('  FAILED   %-34s unexpectedly refused: %s' % (name, e))
        return False


def main():
    if not os.path.isfile(OPAQUE):
        sys.exit('run build_phase5.bat first')
    opaque, alpha = load(OPAQUE), load(ALPHA)
    ok = []

    print('\n-- known-good inputs must still pass --')
    ok.append(expect_ok('rayquery_opaque', opaque))
    ok.append(expect_ok('rayquery_alpha', alpha))
    # The independently written shader reads a RESOURCE inside the Proceed
    # loop, which the isolation check used to refuse. A resource handle is not
    # caller state, so this must be accepted; refusing it would block the most
    # common real alpha test there is.
    if os.path.isfile(INDEP):
        ok.append(expect_ok('rayquery_indep (resource in loop)', load(INDEP)))
    if os.path.isfile(IDS):
        ok.append(expect_ok('rayquery_ids (instance + primitive)', load(IDS)))

    print('\n-- cases the brief says have no valid lowering --')

    # RayQuery in a pixel shader: DispatchRays only launches raygen.
    ok.append(expect_reject(
        'pixel shader', opaque.replace('!{!"cs", i32 6, i32 5}',
                                       '!{!"ps", i32 6, i32 5}'),
        'DispatchRays only launches raygen'))

    # Two concurrent queries: TraceRay has one payload, one in-flight trace.
    two = opaque.replace(
        '  %v33 = call i32 @dx.op.allocateRayQuery(i32 178, i32 1)',
        '  %v33 = call i32 @dx.op.allocateRayQuery(i32 178, i32 1)\n'
        '  %vq2 = call i32 @dx.op.allocateRayQuery(i32 178, i32 1)')
    ok.append(expect_reject('two concurrent queries', two, 'one payload'))

    # groupshared: a raygen shader has no thread group.
    gs = opaque.replace(
        '@dx.op.createHandle(i32 57, i8 1',
        '@dx.op.createHandle(i32 57, i8 1', 1)
    gs = gs.replace('%CB = type {', '@shared = addrspace(3) global [4 x i32] undef\n%CB = type {')
    ok.append(expect_reject('groupshared memory', gs, 'no thread group'))

    # Wave intrinsics change lane occupancy when promoted to raygen.
    wave = opaque.replace(
        '  %v34 = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 %v33)',
        '  %vw = call i32 @dx.op.waveActiveOp.i32(i32 119, i32 %v4, i8 0, i8 0)\n'
        '  %v34 = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 %v33)')
    ok.append(expect_reject('wave intrinsics', wave, 'lane occupancy'))

    print('\n-- shapes this project has not verified --')

    # An opcode we have never seen. Refuse rather than guess: 184 and 185 share
    # one LLVM function, so a near miss here renders the wrong thing silently.
    # 250 is used because it is genuinely unobserved: 191 was this test's
    # example until it turned out to be CandidateTriangleFrontFace.
    # 250 is used because it is genuinely unobserved. This test said 191 until
    # the Unreal survey turned 191 into CandidateTriangleFrontFace, at which
    # point the "unverified" case quietly became a verified one and the check
    # started passing shaders it was written to refuse.
    unknown = opaque.replace(
        '  %v35 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 184, i32 %v33)',
        '  %v35 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 250, i32 %v33)')
    ok.append(expect_reject('unverified rayQuery opcode', unknown,
                            'refusing rather than guessing'))

    # Committed state read inside the loop: the any-hit shader is a separate
    # invocation and cannot see it.
    inloop = alpha.replace(
        '  %v37 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 185, i32 %v33)',
        '  %vct = call float @dx.op.rayQuery_StateScalar.f32(i32 200, i32 %v33)\n'
        '  %v37 = call i32 @dx.op.rayQuery_StateScalar.i32(i32 185, i32 %v33)')
    ok.append(expect_reject('committed state read in loop', inloop,
                            'separate invocation'))

    # No loop and not FORCE_OPAQUE: traversal would yield candidates the
    # shader never inspects, so there is nothing to lower to.
    noflag = opaque.replace('@dx.op.allocateRayQuery(i32 178, i32 1)',
                            '@dx.op.allocateRayQuery(i32 178, i32 0)')
    noflag = noflag.replace('i32 %v33, %dx.types.Handle %v2, i32 1, i32 255',
                            'i32 %v33, %dx.types.Handle %v2, i32 0, i32 255')
    ok.append(expect_reject('no loop, not FORCE_OPAQUE', noflag,
                            'no lowering is defined'))

    # CommittedGeometryIndex is recognised and still has no lowering, because
    # GeometryIndex() in a DXR 1.0 hit shader is ITSELF a Tier 1.1 feature.
    # Measured: such a library sets shader flag 0x2000000 and
    # CreateStateObject on the GTX 1070 returns E_INVALIDARG. Refusing is the
    # only correct answer, so it must not quietly start being accepted.
    if os.path.isfile(IDS):
        geom = load(IDS).replace(
            '@dx.op.rayQuery_StateScalar.i32(i32 207',
            '@dx.op.rayQuery_StateScalar.i32(i32 209')
        ok.append(expect_reject('CommittedGeometryIndex', geom,
                                'itself a Tier 1.1 feature'))

    # Committing BOTH kinds is no longer refused: the loop body lowers twice,
    # into an any-hit and an intersection shader, with two closest-hits because
    # the committed status differs. So this case flips from a refusal to an
    # ACCEPTANCE, and it still earns its place, because mutating the procedural
    # input is the cheapest way to build a both-kinds module and the result has
    # to classify as pattern 5 rather than falling back to 4.
    PROC = os.path.join('phase5', 'cases', 'rayquery_proc.ll')
    if os.path.isfile(PROC):
        # ADD a triangle commit beside the procedural one; replacing it would
        # leave a shader that commits only one kind, which is exactly what the
        # first version of this check did and why it passed nothing.
        mixed = load(PROC)
        m = re.search(r'^.*rayQuery_CommitProceduralPrimitiveHit.*$', mixed, re.M)
        assert m, 'the procedural case no longer has a procedural commit'
        handle = re.search(r'i32 183, i32 (%v\d+)', m.group(0)).group(1)
        mixed = mixed.replace(
            m.group(0),
            m.group(0) + '\n  call void '
            '@dx.op.rayQuery_CommitNonOpaqueTriangleHit(i32 182, i32 %s)' % handle)
        ok.append(expect_pattern('triangle AND procedural commits', mixed, 5))

    print('\n%d of %d checks behaved as intended\n' % (sum(ok), len(ok)))
    return 0 if all(ok) else 1


if __name__ == '__main__':
    sys.exit(main())
