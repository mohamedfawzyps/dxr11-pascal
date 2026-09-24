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
import lower
import nvapi

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


def expect_attrs(name, text):
    """The generated shaders must carry the module's OWN attribute numbers.

    Every shader this project wrote is numbered #0 readnone, #1 nounwind,
    #2 nounwind readonly by DXC, and the lowering hardcoded those. A real
    Unreal module numbers #1 and #2 the other way round and has no #3 at all,
    so AnyHitNull came out marked #3 with no such group, IgnoreHit was
    therefore not noreturn, and the `unreachable` after it was rejected by the
    validator. The message named the instruction, not the attribute.

    Nothing in the suite could catch that, because every input here agrees
    with the hardcoded numbers. This rewrites them the other way round first.
    """
    swapped = (text.replace('attributes #1 = { nounwind }',
                            'attributes #1 = { nounwind readonly }')
                   .replace('attributes #2 = { nounwind readonly }',
                            'attributes #2 = { nounwind }'))
    if swapped == text:
        print('  FAILED   %-34s the input did not have the numbering this '
              'rewrites; the check applied to nothing' % name)
        return False
    m = Module(swapped)
    q = rayquery.analyze(m)
    out = lower.lower(m, q)

    groups = dict((b.strip(), int(n))
                  for n, b in re.findall(
                      r'(?m)^attributes #(\d+) = \{ (.*) \}$', out))
    want = {'AnyHitNull': 'noreturn nounwind',
            'AnyHit': 'nounwind',
            'ClosestHit': 'nounwind'}
    for fn, body in want.items():
        if body not in groups:
            print('  FAILED   %-34s no attribute group for %r' % (name, body))
            return False
        mm = re.search(r'(?m)^define void @%s\(.*\) #(\d+) \{$' % fn, out)
        if not mm:
            continue
        if int(mm.group(1)) != groups[body]:
            print('  FAILED   %-34s %s is #%s, but %r is #%d'
                  % (name, fn, mm.group(1), body, groups[body]))
            return False
    print('  ok       %-34s attribute groups resolved, not assumed' % name)
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


def expect_lower_reject(name, text, must_mention):
    """Refused by the LOWERING rather than by the analysis.

    Some refusals live in lower.py, because they are about whether the loop
    body can be transplanted rather than about what the query does. They were
    not covered here at all until a new one needed proving reachable.
    """
    try:
        m = Module(text)
        q = rayquery.analyze(m)
        lower.lower(m, q)
        print('  FAILED   %-34s was LOWERED, should have been refused' % name)
        return False
    except rayquery.Unsupported as e:
        if must_mention.lower() not in str(e).lower():
            print('  FAILED   %-34s refused for the wrong reason: %s' % (name, e))
            return False
        print('  refused  %-34s %s' % (name, str(e).split(';')[0][:52]))
        return True


def expect_lower_ok(name, text):
    """LOWERED, where it used to be refused."""
    try:
        m = Module(text)
        lower.lower(m, rayquery.analyze(m))
        print('  ok       %-34s lowered' % name)
        return True
    except rayquery.Unsupported as e:
        print('  FAILED   %-34s refused: %s' % (name, e))
        return False


DXRW = os.path.join('phase5out', 'dxrw.exe')


def cpp_agrees(name, text, must_mention=None, forbid=None):
    """The C++ port gives the same verdict. Byte-identity compares OUTPUTS, so
    it cannot see a refusal one side has and the other lacks; that is how the
    C++ once lost a Python refusal and nothing noticed. None means it must
    lower, and `forbid`, a pattern, must then not appear in what it wrote."""
    if not os.path.isfile(DXRW):
        print('  FAILED   %-34s %s missing, build_rewriter.bat' % (name, DXRW))
        return False
    import subprocess
    import tempfile
    d = tempfile.mkdtemp()
    src, dst = os.path.join(d, 'in.ll'), os.path.join(d, 'out.ll')
    with io.open(src, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    r = subprocess.run([DXRW, 'lower', src, dst], capture_output=True, text=True)
    said = (r.stdout + r.stderr).strip()
    if must_mention is None:
        good = r.returncode == 0
        if good and forbid:
            with io.open(dst, encoding='utf-8') as f:
                left = re.search(forbid, f.read())
            if left:
                good, said = False, 'left in place: ' + left.group(0)
    else:
        good = r.returncode != 0 and must_mention.lower() in said.lower()
    print('  %s %-34s C++ %s' % ('ok      ' if good else 'FAILED  ', name,
                                 'agrees' if good else 'disagrees: ' + said[:80]))
    return good


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
    # This was "two concurrent queries" until 0.43.0, which lowers several
    # queries in one entry point. The mutation adds an allocation that is never
    # traced, which is still refused, for that reason: the sixth test here
    # whose premise moved.
    ok.append(expect_reject('a second query never traced', two, 'never traced'))

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

    # CommittedGeometryIndex FLIPS from a refusal to an acceptance, and the
    # case is kept rather than deleted so the refusal cannot creep back.
    #
    # It was refused for most of this project's life, and the measurement
    # behind that is still true: GeometryIndex() in a DXR 1.0 hit shader is
    # ITSELF a Tier 1.1 feature, such a library sets shader flag 0x2000000, and
    # CreateStateObject on the GTX 1070 returns E_INVALIDARG. What changed is
    # that the answer no longer comes from an intrinsic. The shim builds the
    # shader table, so the geometry index rides in the record as a local root
    # signature constant and the hit shader reads it back. Measured at
    # SFI0 = 0x0: phase5/cases/reference/lib_localroot_ref.hlsl.
    #
    # This is the third time here that a test's premise was a fact about the
    # world and the fact moved. The suite caught it, which is the point.
    if os.path.isfile(IDS):
        geom = load(IDS).replace(
            '@dx.op.rayQuery_StateScalar.i32(i32 207',
            '@dx.op.rayQuery_StateScalar.i32(i32 209')
        ok.append(expect_ok('CommittedGeometryIndex now lowers', geom))

    # Attribute group numbers are the module's, not a constant.
    ok.append(expect_attrs('attribute numbering differs', alpha))

    # A UAV read before the loop, used inside it. This was the BOUNDARY of the
    # recomputable exemption, and a refusal, until 0.42.0: running the load
    # again in the hit shader is not the same read, because another thread may
    # write that location meanwhile (this one reads outBuf[0], which thread 0
    # writes). So the value is not re-read: it is CARRIED in the payload, the
    # value the raygen read, once. The fifth test here whose premise moved;
    # it is kept and flipped, and checks the any-hit takes the value from the
    # payload rather than loading the buffer again.
    UAV = os.path.join('phase5', 'cases', 'refuse_uav_in_loop.ll')
    if os.path.isfile(UAV):
        t = load(UAV)
        name = 'UAV read before the loop, carried'
        try:
            m = Module(t)
            out = lower.lower(m, rayquery.analyze(m))
            ah = re.search(r'(?s)define void @AnyHit\(.*?\n\}', out).group(0)
            carried = re.search(r'= load float, float\* %rq\.pac11', ah)
            reread = 'rawBufferLoad' in ah or 'bufferLoad' in ah
            good = bool(carried) and not reread
            print('  %s %-34s %s' % ('ok      ' if good else 'FAILED  ', name,
                                     'from payload field 11' if good else
                                     'carried=%s re-read=%s' % (bool(carried), reread)))
            ok.append(good)
        except rayquery.Unsupported as e:
            print('  FAILED   %-34s refused: %s' % (name, e))
            ok.append(False)
        ok.append(cpp_agrees(name, t, None))

    # Several queries in one entry point (0.43.0): each its own TraceRay and
    # payload, ONE any-hit choosing the loop body from the query id the raygen
    # stored. The two loop bodies commit opposite triangle halves, so running
    # the wrong one changes the render, which the dispatch suite compares
    # against WARP; here, the structure and the C++ verdict.
    TWOQ = os.path.join('phase5', 'cases', 'rayquery_twoq_sm66.ll')
    if os.path.isfile(TWOQ):
        t = load(TWOQ)
        name = 'two queries in sequence'
        try:
            m = Module(t)
            out = lower.lower(m, rayquery.analyze(m))
            ids = re.findall(r'store i32 (\d), i32\* %rq(?:\.q\d)?\.plq', out)
            ah = re.search(r'(?s)define void @AnyHit\(.*?\n\}', out).group(0)
            good = (ids == ['0', '1'] and '%rq.qid = load i32' in ah
                    and out.count('call void @dx.op.traceRay.') == 2)
            print('  %s %-34s %s' % ('ok      ' if good else 'FAILED  ', name,
                                     'two traces, ids 0 and 1, one any-hit' if good
                                     else 'ids=%s' % ids))
            ok.append(good)
        except rayquery.Unsupported as e:
            print('  FAILED   %-34s refused: %s' % (name, e))
            ok.append(False)
        ok.append(cpp_agrees(name, t, None))
    for f, why in (('refuse_nested_query.ll', 'cannot call TraceRay'),
                   ('refuse_twoq_isect.ll', 'which query it serves')):
        path = os.path.join('phase5', 'cases', f)
        if os.path.isfile(path):
            t = load(path)
            name = 'query nested in a loop' if 'nested' in f else 'two queries, one intersection'
            ok.append(expect_reject(name, t, why))
            ok.append(cpp_agrees(name, t, why))

    # Where the payload cannot carry it, refused by name: a loop body that
    # becomes an INTERSECTION shader has no payload in DXR 1.0, and a value
    # computed after TraceRayInline does not exist yet when the raygen fills
    # the payload.
    for f, why in (('refuse_carry_isect.ll', 'has no payload'),
                   ('refuse_carry_after.ll', 'computed after the trace')):
        path = os.path.join('phase5', 'cases', f)
        if os.path.isfile(path):
            t = load(path)
            name = 'carry ' + ('into an intersection' if 'isect' in f else 'after the trace')
            ok.append(expect_lower_reject(name, t, why))
            ok.append(cpp_agrees(name, t, why))

    # A Proceed loop body that WRITES.
    #
    # The check above guards what the body READS. This is the other half, and
    # it went missing for the whole of Phase 5 because every case in this suite
    # has a pure loop body. A real Unreal shader, RayTracingDebugMainCS, does
    # not: it appends a debug record per candidate, and the lowering
    # transplanted the append into the any-hit shader, where it runs a
    # different number of times and in a different order.
    #
    # 0.41.0 narrowed it to what an APPEND needs: a counter update, and stores
    # indexed by the value it returned. The mutations add them on %v2, the
    # handle the loop body ALREADY reads its alpha mask through, so the
    # isolation check exempts it and the side-effect rule is what decides.
    # Pointing one at a UAV defined outside the loop made refuse_uav_in_loop
    # fire instead in the first version of this test, which is how the two
    # were told apart. The attribute group is looked up rather than written as
    # #1, which is the mistake 0.26.0 had to fix once already. Every verdict
    # is checked in the C++ too.
    SFX = os.path.join('phase5', 'cases', 'rayquery_indep.ll')
    if os.path.isfile(SFX):
        base = load(SFX)
        g = re.search(r'^attributes #(\d+) = \{ nounwind \}\s*$', base, re.M)
        assert g, 'no bare nounwind attribute group to hang a writing op on'
        commit = re.search(r'^.*rayQuery_CommitNonOpaqueTriangleHit.*$', base, re.M)
        assert commit, 'the independent case no longer commits inside the loop'
        decl = ('declare i32 @dx.op.bufferUpdateCounter(i32, %%dx.types.Handle, i8) #%s\n\n%s'
                % (g.group(1), g.group(0)))
        counter = ('  %sidefx = call i32 @dx.op.bufferUpdateCounter'
                   '(i32 70, %dx.types.Handle %v2, i8 1)\n')

        def store(index):
            return ('  call void @dx.op.rawBufferStore.i32(i32 140, %%dx.types.Handle %%v2, '
                    'i32 %s, i32 0, i32 94, i32 undef, i32 undef, i32 undef, i8 1, i32 4)\n'
                    % index)

        def mutate(extra):
            t = base.replace(commit.group(0), counter + extra + commit.group(0))
            return t.replace(g.group(0), decl)

        for name, text, why in (
                ('counter updated inside the loop', mutate(''), None),
                ('append at the counter\'s index', mutate(store('%sidefx')), None),
                ('store at a FIXED index in the loop', mutate(store('0')),
                 'has a side effect')):
            if why is None:
                ok.append(expect_lower_ok(name, text))
            else:
                ok.append(expect_lower_reject(name, text, why))
            ok.append(cpp_agrees(name, text, why))

    # A call with a METADATA ATTACHMENT, `, !dx.precise !N`, which DXC emits for
    # a `precise` value. The call pattern ended at the closing parenthesis, so
    # such a call was not seen as a call at all: a RayQuery accessor carrying
    # one was never rewritten and still named the deleted query handle, and the
    # assembler said "use of undefined value". 17 of Unreal's MegaLights and
    # Lumen shaders, hidden until 0.41.0 behind the side-effect refusal, and
    # probably the one assembler failure the brief could not explain.
    if os.path.isfile(SFX):
        t = load(SFX)
        rayt = re.search(r'^.*rayQuery_StateScalar\.f32\(i32 200, i32 %v\d+\)', t, re.M)
        ids = [int(x) for x in re.findall(r'^!(\d+) = ', t, re.M)]
        assert rayt and ids, 'the independent case no longer reads CommittedRayT'
        md = max(ids) + 1
        t = t.replace(rayt.group(0), rayt.group(0) + ', !dx.precise !%d' % md, 1)
        t = t.rstrip('\n') + '\n!%d = !{i32 1}\n' % md
        name = 'accessor with !dx.precise attached'
        try:
            m = Module(t)
            out = lower.lower(m, rayquery.analyze(m))
            left = re.search(r'rayQuery_\w+[.\w]*\(i32 \d+', out)
            if left:
                print('  FAILED   %-34s left in place: %s' % (name, left.group(0)))
                ok.append(False)
            else:
                print('  ok       %-34s rewritten, nothing left behind' % name)
                ok.append(True)
        except rayquery.Unsupported as e:
            print('  FAILED   %-34s refused: %s' % (name, e))
            ok.append(False)
        ok.append(cpp_agrees(name, t, None, r'rayQuery_\w+[.\w]*\(i32 \d+'))

    # Around an append, Abort() and an intersection shader stay refused: after
    # an abort the lowering lets traversal continue, and an intersection
    # shader may run more than once per primitive whatever the flags say.
    for path, anchor, why in (
            (os.path.join('phase5', 'cases', 'rayquery_abort.ll'),
             r'^.*rayQuery_Abort\(i32 181.*$', 'calls Abort()'),
            (os.path.join('phase5', 'cases', 'rayquery_proc.ll'),
             r'^.*rayQuery_CommitProceduralPrimitiveHit\(i32 183.*$',
             'intersection shader')):
        if not os.path.isfile(path):
            continue
        t = load(path)
        g = re.search(r'^attributes #(\d+) = \{ nounwind \}\s*$', t, re.M)
        uav = re.search(r'^\s*(%v\d+) = call %dx\.types\.Handle @dx\.op\.createHandle'
                        r'\(i32 57, i8 1,', t, re.M)
        at = re.search(anchor, t, re.M)
        assert g and uav and at, 'cannot build the append mutation of ' + path
        t = t.replace(at.group(0),
                      '  %%sidefx = call i32 @dx.op.bufferUpdateCounter(i32 70, '
                      '%%dx.types.Handle %s, i8 1)\n%s' % (uav.group(1), at.group(0)))
        if 'declare i32 @dx.op.bufferUpdateCounter' not in t:
            t = t.replace(g.group(0),
                          'declare i32 @dx.op.bufferUpdateCounter(i32, %%dx.types.Handle, i8) #%s'
                          '\n\n%s' % (g.group(1), g.group(0)))
        name = 'append with ' + ('Abort()' if 'abort' in path else 'an intersection')
        ok.append(expect_lower_reject(name, t, why))
        ok.append(cpp_agrees(name, t, why))

    # NVAPI shader extension calls (0.41.1). What 0.41.0 took for an append is
    # NVIDIA's encoding of an intrinsic: stores to a RWStructuredBuffer of
    # NvShaderExtnStruct, which the driver reads as a RayQuery cluster-ID call
    # while Unreal has the slot registered. Moved into an any-hit it killed the
    # driver. Ops 94 and 95 fold to 0xFFFFFFFF, and nothing of the call may be
    # left; any other op is refused, by both implementations.
    NV = os.path.join('phase5', 'cases', 'rayquery_nvapi_sm66.ll')
    if os.path.isfile(NV):
        t = load(NV)

        def nv_lower(text):
            m = Module(nvapi.fold(text))
            return lower.lower(m, rayquery.analyze(m))

        name = 'NVAPI cluster ID calls folded'
        try:
            out = nv_lower(t)
            left = re.search(r'bufferUpdateCounter|i32 0, i32 9[45],', out)
            folded = re.search(r'icmp eq i32 -1, -1', out) and re.search(r'uitofp i32 -1 ', out)
            if left or not folded:
                print('  FAILED   %-34s %s' % (name, 'left in place: ' + left.group(0) if left
                                               else 'the constant is not where the calls were'))
                ok.append(False)
            else:
                print('  ok       %-34s both calls are 0xFFFFFFFF' % name)
                ok.append(True)
        except rayquery.Unsupported as e:
            print('  FAILED   %-34s refused: %s' % (name, e))
            ok.append(False)
        ok.append(cpp_agrees(name, t, None, r'bufferUpdateCounter|NvShaderExtnStruct"\* undef'))

        for name, text, why in (
                ('NVAPI op other than cluster ID',
                 t.replace('i32 0, i32 94,', 'i32 0, i32 50,', 1), 'op 50'),
                ('NVAPI call without its result',
                 re.sub(r'(?m)^(\s*)(%v\d+) = (call i32 @dx\.op\.bufferUpdateCounter\(i32 70, '
                        r'%dx\.types\.Handle %v\d+, i8 1\)(?:(?!\n).)*\n)((?:(?!bufferUpdateCounter).*\n)*?)'
                        r'\s*%v\d+ = call i32 @dx\.op\.bufferUpdateCounter.*\n',
                        r'\1\2 = \3\4', t, count=1), 'op 94')):
            assert text != t, 'cannot build the mutation: ' + name
            try:
                nv_lower(text)
                print('  FAILED   %-34s was LOWERED, should have been refused' % name)
                ok.append(False)
            except rayquery.Unsupported as e:
                good = why.lower() in str(e).lower()
                print('  %s %-34s %s' % ('refused ' if good else 'FAILED  ', name, str(e)[:52]))
                ok.append(good)
            ok.append(cpp_agrees(name, text, why))

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

    # A resource handle indexed DYNAMICALLY and used inside the Proceed loop.
    # The handle exemption in the isolation check is only sound when the module
    # fully determines the handle; a dynamic index makes it depend on a value
    # computed in the raygen, which the any-hit cannot see. Provoked from the
    # independent shader, which is the one that genuinely reads a resource in
    # its loop body.
    if os.path.isfile(INDEP):
        dyn = load(INDEP)
        m = re.search(r'^.*dx\.op\.createHandle\(i32 57, i8 0.*$', dyn, re.M)
        assert m, 'the independent case no longer creates an SRV handle'
        dyn = dyn.replace(m.group(0),
                          re.sub(r'(i32 57, i8 0, i32 \d+), i32 \d+',
                                 r'\1, i32 %v1', m.group(0)))
        ok.append(expect_lower_reject('dynamic handle used in the loop', dyn,
                                      'indexed dynamically'))

    print('\n%d of %d checks behaved as intended\n' % (sum(ok), len(ok)))
    return 0 if all(ok) else 1


if __name__ == '__main__':
    sys.exit(main())
