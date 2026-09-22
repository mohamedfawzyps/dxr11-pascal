#!/usr/bin/env python3
"""Give every unnamed LLVM value and block an explicit name.

Pattern 1 ran into this: LLVM numbers unnamed values and blocks sequentially,
so the sequence has to stay contiguous. Inserting a numbered value shifts
everything after it, and DELETING one makes the assembler refuse outright with
"instruction expected to be numbered '%33'".

Pattern 1 could dodge that, because it only replaced instructions one for one.
Pattern 3 cannot: lowering the Proceed loop into an any-hit shader DELETES six
whole basic blocks, which would renumber most of the function.

So normalise first. Once %33 is %v33 and block 44 is bb44, nothing is
positional any more and instructions and blocks can be added or removed freely.
Anything the rewriter eventually does will want this same pass.

    %33 = call ...        ->  %v33 = call ...
    ; <label>:44          ->  bb44:
    br i1 %45, label %36  ->  br i1 %v45, label %bb36
"""

import re

# A block label definition, with the predecessor comment DXC leaves behind.
_LABEL = re.compile(r'^; <label>:(\d+)(\s*); preds = (.*)$')
_LABEL_NOPRED = re.compile(r'^; <label>:(\d+)\s*$')


def normalize(text):
    """Rename unnamed values to %vN and unnamed blocks to bbN, in function
    bodies only. Metadata and the disassembly's comment header are untouched.

    The ENTRY block is the awkward one. DXC gives it no label line, because
    nothing can branch to it, so there is nothing here to rename. But a phi may
    still name it as a PREDECESSOR: an `if` with no `else` straight out of the
    entry block produces `phi [ 0.0, %0 ], [ 0.5, %9 ]`. That %0 becomes %bb0,
    nothing then defines bb0, and the assembler says "use of undefined value
    '%bb0'".

    So when a function turns out to reference its entry block, a label is added
    for it. That is legal: an entry block may be named, it just cannot be
    branched to. It is added only when referenced, so a module that never
    needed one is byte-for-byte unchanged.

    This is why every case in this project opened with
    `if (tid.x >= width) return;` for so long without anyone noticing. That
    guard puts a block between the entry and everything else, so the entry is
    never a phi predecessor.
    """
    out = []
    in_body = False
    body_start = -1          # index in `out` of the `define` line
    saw_entry_label = False
    refs_entry = False
    for line in text.split('\n'):
        if line.startswith('define '):
            in_body = True
            body_start = len(out)
            saw_entry_label = False
            refs_entry = False
            out.append(line)
            continue
        if in_body and line == '}':
            in_body = False
            if refs_entry and not saw_entry_label:
                out.insert(body_start + 1, 'bb0:')
            out.append(line)
            continue
        if not in_body:
            out.append(line)
            continue

        # Block definitions. The preds list is only a comment, but leaving it
        # stale would make the file misleading to read, so rename it too.
        m = _LABEL.match(line)
        if m:
            if m.group(1) == '0':
                saw_entry_label = True
            preds = re.sub(r'%(\d+)', r'%bb\1', m.group(3))
            out.append('bb%s:%s; preds = %s' % (m.group(1), m.group(2), preds))
            continue
        m = _LABEL_NOPRED.match(line)
        if m:
            if m.group(1) == '0':
                saw_entry_label = True
            out.append('bb%s:' % m.group(1))
            continue

        # Block references in terminators, then in phi incoming pairs. Both
        # must happen before the catch-all below, which assumes what is left
        # is a value.
        line = re.sub(r'label %(\d+)', r'label %bb\1', line)
        if ' = phi ' in line:
            line = re.sub(r', %(\d+) \]', r', %bb\1 ]', line)
        if re.search(r'%bb0\b', line):
            refs_entry = True

        # Everything still numbered is a value. Named types like
        # %dx.types.Handle and %"class.Foo" start with a letter or a quote, so
        # they cannot match.
        line = re.sub(r'%(\d+)\b', r'%v\1', line)
        out.append(line)
    return '\n'.join(out)

