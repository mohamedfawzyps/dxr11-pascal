"""Bake the shader-record constants into the hit shaders as immediates.

WHY THIS EXISTS. The lowering delivers GeometryIndex and
InstanceContributionToHitGroupIndex through a cbuffer bound by a LOCAL root
signature, b0 space1, which the shim fills per shader table record. On the GTX
1070 a hit shader that READS such a cbuffer makes the driver access-violate
inside CreateStateObject, intermittently, on a cold compile. Measured, see
phase5/cases/driver-crash/README.md: a root CBV descriptor does not help and a
different register space does not help. Creating the local root signature is
fine; only the read is fatal.

So the read is removed rather than dodged. This pass takes the lowered text and
the list of (geometryIndex, instanceContribution) pairs the scene's records
need, and emits one copy of every hit shader per pair, each with its pair
folded in as a constant. The shim then builds one hit group per pair and points
each record at the group carrying its numbers. The record carries nothing but
an identifier, and there is no local root signature at all.

It is a separate pass, not part of lower(), because lowering is per SHADER and
the pairs are per SCENE: the acceleration structures do not exist when a
pipeline is created, so the shim lowers once, bakes with a placeholder pair,
and bakes again at dispatch once it knows what the records need. lower() is
unchanged, so nothing that does not read the record moves by a byte.

The C++ port is proxy/rewriter/rq_bake.cpp and must produce identical text.
"""

import re

# The hit shaders that get one copy per pair, in emission order. Every one of
# them that the lowered module defines is copied, whether or not it reads the
# record itself, so a hit group's shaders always carry the same suffix. The
# rejecting stubs, the raygen and the miss never read the record and are left
# alone.
HIT_FUNCTIONS = ('AnyHit', 'ClosestHit', 'Isect', 'ClosestHitProc')

# A generous ceiling rather than a measured one. 64 copies were measured clean
# on the 1070, at about 34 ms of cold compile per pair; this is where a scene
# with pathologically many (geometry, contribution) pairs stops being served
# rather than stalling the application for a minute.
MAX_PAIRS = 1024


class BakeError(Exception):
    pass


def copy_name(name, k):
    return '%s_%d' % (name, k)


def _record_tags(lines):
    """The tag of every record read, from its load line.

    lower.py emits each read as four lines named %rq.cbv<tag>, %rq.cbh<tag>,
    %rq.cbr<tag> and then an extractvalue out of %rq.cbr<tag>. The load is the
    only line that names the record TYPE, so it is the one to key on."""
    tags = []
    for l in lines:
        m = re.match(r'\s*%rq\.cbv([\w.]+) = load %rq_record, ', l)
        if m:
            tags.append(m.group(1))
    return tags


def _function_span(lines, name):
    """(first, last) line index of `define ... @name(` .. `}`, or None."""
    head = '@%s(' % name
    for i, l in enumerate(lines):
        if l.startswith('define ') and head in l:
            for j in range(i + 1, len(lines)):
                if lines[j] == '}':
                    return i, j
            raise BakeError('function %s has no closing brace' % name)
    return None


def _bake_body(body, name, k, pair, tags):
    out = []
    drop = set()
    for t in tags:
        drop.add('%rq.cbv' + t)
        drop.add('%rq.cbh' + t)
        drop.add('%rq.cbr' + t)
    for l in body:
        if l.startswith('define '):
            out.append(l.replace('@%s(' % name, '@%s(' % copy_name(name, k), 1))
            continue
        m = re.match(r'(\s*)(%[\w.]+) = ', l)
        if m and m.group(2) in drop:
            continue
        m = re.match(r'(\s*)(%[\w.]+) = extractvalue %dx\.types\.CBufRet\.i32 '
                     r'%rq\.cbr([\w.]+), (\d+)\s*$', l)
        if m and m.group(3) in tags:
            word = int(m.group(4))
            if word not in (0, 1):
                raise BakeError('record read of word %d; the record has two' % word)
            out.append('%s%s = add i32 0, %d' % (m.group(1), m.group(2), pair[word]))
            continue
        out.append(l)
    return out


def _prune_declares(lines):
    """Drop a declare the baking made dead.

    The lowered module validated, so it had no unused declaration; any unused
    one now is a record read's, and an unused declaration is itself a
    validation error."""
    body = '\n'.join(l for l in lines if not l.startswith('declare '))
    out = []
    for l in lines:
        if l.startswith('declare '):
            m = re.search(r'@([\w.$"]+)\(', l)
            if m and ('@' + m.group(1) + '(') not in body:
                continue
        out.append(l)
    return out


def _drop_record_resource(lines):
    """Take the record cbuffer out of !dx.resources."""
    rec = None
    for l in lines:
        m = re.match(r'!(\d+) = !\{i32 \d+, %rq_record\* @rq_record, ', l)
        if m:
            rec = '!' + m.group(1)
    if rec is None:
        raise BakeError('no resource record for @rq_record')

    res_tuple = None
    for l in lines:
        m = re.match(r'!dx\.resources = !\{(!\d+)\}$', l)
        if m:
            res_tuple = m.group(1)

    out = []
    emptied = None
    lists = 0
    for l in lines:
        if l.startswith(rec + ' = '):
            continue
        m = re.match(r'(!\d+) = !\{((?:!\d+)(?:, !\d+)*)\}$', l)
        if m and m.group(1) != res_tuple:
            items = [x.strip() for x in m.group(2).split(',')]
            if rec in items:
                lists += 1
                items = [x for x in items if x != rec]
                if not items:
                    emptied = m.group(1)
                    continue
                out.append('%s = !{%s}' % (m.group(1), ', '.join(items)))
                continue
        out.append(l)
    if lists != 1:
        raise BakeError('the record cbuffer is in %d resource lists, expected 1' % lists)

    if emptied:
        # The record was the only cbuffer, so the class becomes null, which is
        # how DXC writes a resource class a module does not have.
        fixed = []
        for l in out:
            head = (res_tuple or '') + ' = !{'
            if res_tuple and l.startswith(head) and l.endswith('}'):
                items = [x.strip() for x in l[len(head):-1].split(',')]
                items = ['null' if x == emptied else x for x in items]
                l = head + ', '.join(items) + '}'
            fixed.append(l)
        out = fixed
    return out


def bake(text, pairs, names=HIT_FUNCTIONS):
    """Return `text` with the record reads baked, one hit shader copy per pair."""
    pairs = [(int(g), int(c)) for g, c in pairs]
    if not pairs:
        raise BakeError('no pairs to bake')
    if len(pairs) > MAX_PAIRS:
        raise BakeError('%d distinct (geometry, contribution) pairs; the ceiling '
                        'is %d' % (len(pairs), MAX_PAIRS))
    for g, c in pairs:
        if not (0 <= g < 2 ** 31 and 0 <= c < 2 ** 31):
            raise BakeError('pair (%d, %d) does not fit an i32 immediate' % (g, c))
    if len(set(pairs)) != len(pairs):
        raise BakeError('pairs must be distinct')

    lines = text.split('\n')
    tags = _record_tags(lines)
    if not tags:
        raise BakeError('the module reads no record constants; nothing to bake')

    # 1. the hit shaders, one copy per pair, in place of the original
    present = []
    for name in names:
        span = _function_span(lines, name)
        if span is None:
            continue
        present.append(name)
        s, e = span
        body = lines[s:e + 1]
        copies = []
        for k, pair in enumerate(pairs):
            if k:
                copies.append('')
            copies += _bake_body(body, name, k, pair, tags)
        lines = lines[:s] + copies + lines[e + 1:]
    if not present:
        raise BakeError('the module defines none of the hit shaders')

    # 2. the record itself: its type, its global, its resource record
    lines = [l for l in lines
             if not l.startswith('%rq_record = type ')
             and not l.startswith('@rq_record = ')]
    lines = _drop_record_resource(lines)
    lines = _prune_declares(lines)

    # 3. entry points and type annotations, one per copy
    ids = [int(x) for x in re.findall(r'^!(\d+) = ', '\n'.join(lines), re.M)]
    nxt = max(ids) + 1
    added = []            # new metadata lines, appended at the end
    ep_extra = {}         # original entry node -> the nodes of copies 1..N-1
    ta_line = None
    for l in lines:
        m = re.match(r'!dx\.typeAnnotations = !\{(!\d+)', l)
        if m:
            ta_line = m.group(1)
    for name in present:
        found = False
        for i, l in enumerate(lines):
            m = re.match(r'(!\d+) = !\{(.+)\* @%s, !"%s", (.*)\}$'
                         % (re.escape(name), re.escape(name)), l)
            if not m:
                continue
            found = True
            node, ty, rest = m.group(1), m.group(2), m.group(3)
            lines[i] = '%s = !{%s* @%s, !"%s", %s}' % (
                node, ty, copy_name(name, 0), copy_name(name, 0), rest)
            extra = []
            for k in range(1, len(pairs)):
                added.append('!%d = !{%s* @%s, !"%s", %s}' % (
                    nxt, ty, copy_name(name, k), copy_name(name, k), rest))
                extra.append('!%d' % nxt)
                nxt += 1
            ep_extra[node] = extra
            break
        if not found:
            raise BakeError('no entry point for %s' % name)

    for i, l in enumerate(lines):
        if l.startswith('!dx.entryPoints = !{'):
            items = [x.strip() for x in l[len('!dx.entryPoints = !{'):-1].split(',')]
            grown = []
            for it in items:
                grown.append(it)
                grown += ep_extra.get(it, [])
            lines[i] = '!dx.entryPoints = !{%s}' % ', '.join(grown)
        elif ta_line and l.startswith(ta_line + ' = !{'):
            for name in present:
                pat = re.compile(r'(void \([^()]*\)\*) @%s, (!\d+)' % re.escape(name))
                m = pat.search(l)
                if not m:
                    raise BakeError('no type annotation for %s' % name)
                rep = ', '.join('%s @%s, %s' % (m.group(1), copy_name(name, k), m.group(2))
                                for k in range(len(pairs)))
                l = l[:m.start()] + rep + l[m.end():]
            lines[i] = l

    # appended after the last metadata line, before any trailing blank lines
    end = len(lines)
    while end and lines[end - 1] == '':
        end -= 1
    lines = lines[:end] + added + lines[end:]

    out = '\n'.join(lines)
    left = [l for l in out.split('\n')
            if not l.lstrip().startswith(';')
            and ('rq_record' in l or '%rq.cbr' in l)]
    if left:
        raise BakeError('a record read survived the baking: %s' % left[0].strip())
    return out


def parse_pairs(s):
    """'g:c,g:c' -> [(g, c), ...]"""
    out = []
    for item in s.split(','):
        g, c = item.split(':')
        out.append((int(g), int(c)))
    return out
