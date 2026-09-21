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
    bodies only. Metadata and the disassembly's comment header are untouched."""
    out = []
    in_body = False
    for line in text.split('\n'):
        if line.startswith('define '):
            in_body = True
            out.append(line)
            continue
        if in_body and line == '}':
            in_body = False
            out.append(line)
            continue
        if not in_body:
            out.append(line)
            continue

        # Block definitions. The preds list is only a comment, but leaving it
        # stale would make the file misleading to read, so rename it too.
        m = _LABEL.match(line)
        if m:
            preds = re.sub(r'%(\d+)', r'%bb\1', m.group(3))
            out.append('bb%s:%s; preds = %s' % (m.group(1), m.group(2), preds))
            continue
        m = _LABEL_NOPRED.match(line)
        if m:
            out.append('bb%s:' % m.group(1))
            continue

        # Block references in terminators, then in phi incoming pairs. Both
        # must happen before the catch-all below, which assumes what is left
        # is a value.
        line = re.sub(r'label %(\d+)', r'label %bb\1', line)
        if ' = phi ' in line:
            line = re.sub(r', %(\d+) \]', r', %bb\1 ]', line)

        # Everything still numbered is a value. Named types like
        # %dx.types.Handle and %"class.Foo" start with a letter or a quote, so
        # they cannot match.
        line = re.sub(r'%(\d+)\b', r'%v\1', line)
        out.append(line)
    return '\n'.join(out)

