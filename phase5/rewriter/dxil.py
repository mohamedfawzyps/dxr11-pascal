#!/usr/bin/env python3
"""A deliberately small model of a DXIL .ll module.

This is NOT a general LLVM parser and must not grow into one. It understands
exactly what the RayQuery lowering needs to reason about, and keeps everything
else as verbatim text so it survives untouched:

  - functions, their basic blocks, and the instructions in them
  - the result name of an instruction, so def-use chains can be followed
  - dx.op calls decoded into (opcode number, arguments)
  - branch targets and phi incoming edges, so the CFG is real
  - dominators and natural loops, because `while (q.Proceed())` is loop-rotated
    and cannot be recognised any other way

Everything is keyed off names rather than positions. Run llnorm first: LLVM
numbers unnamed values and blocks positionally, and this model assumes it can
insert and delete without renumbering anything.
"""

import re

# --- small text helpers ----------------------------------------------------


def split_args(text):
    """Split a call's argument list on top-level commas.

    Types like <2 x float> and %"class.Foo<Bar>" contain commas and brackets,
    so a plain split is wrong."""
    args, depth, cur, quote = [], 0, [], False
    for ch in text:
        if quote:
            cur.append(ch)
            if ch == '"':
                quote = False
            continue
        if ch == '"':
            quote = True
            cur.append(ch)
        elif ch in '([{<':
            depth += 1
            cur.append(ch)
        elif ch in ')]}>':
            depth -= 1
            cur.append(ch)
        elif ch == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if ''.join(cur).strip():
        args.append(''.join(cur).strip())
    return args


def operand_name(arg):
    """'%dx.types.Handle %v2' -> '%v2'; 'i32 178' -> None."""
    m = re.search(r'(%[\w.$-]+|%"[^"]+")\s*$', arg.strip())
    return m.group(1) if m else None


# --- model -----------------------------------------------------------------

_RESULT = re.compile(r'^\s*(%[\w.$-]+)\s*=\s*(.*)$')
_CALL = re.compile(r'\bcall\s+.*?@([\w.$"<>-]+|"[^"]+")\s*\((.*)\)\s*(?:#\d+)?\s*$')
_BR_COND = re.compile(r'^\s*br\s+i1\s+(\S+),\s*label\s+(%[\w.$-]+),\s*label\s+(%[\w.$-]+)')
_BR_UNCOND = re.compile(r'^\s*br\s+label\s+(%[\w.$-]+)')
_PHI_IN = re.compile(r'\[\s*([^,\]]+),\s*(%[\w.$-]+)\s*\]')


class Instr(object):
    """One instruction, kept as text plus whatever was worth decoding.

    `index` is the line number in the module, so a transform can edit by
    position instead of by matching text. Matching text is what the hand
    lowerings did, and it is exactly what does not generalise."""

    def __init__(self, line, index=-1):
        self.line = line
        self.index = index
        self.result = None
        self.body = line.strip()
        m = _RESULT.match(line)
        if m:
            self.result, self.body = m.group(1), m.group(2).strip()

        # Strip the trailing ; comment DXC emits, but keep it on the line.
        body = self.body
        cut = _strip_comment(body)

        self.callee = None
        self.args = []
        m = _CALL.search(cut)
        if m:
            self.callee = m.group(1).strip('"')
            self.args = split_args(m.group(2))

        self.is_phi = cut.startswith('phi ')
        self.phi_incoming = _PHI_IN.findall(cut) if self.is_phi else []

    @property
    def dxop(self):
        """The dx.op opcode number, or None. Operand 0 of every dx.op call is
        the opcode immediate, and it is the ONLY reliable discriminator: DXC
        reuses one LLVM function for several opcodes, so 184 (CommittedStatus)
        and 185 (CandidateType) are both dx.op.rayQuery_StateScalar.i32."""
        if not self.callee or not self.callee.startswith('dx.op.') or not self.args:
            return None
        m = re.match(r'^i32\s+(\d+)$', self.args[0].strip())
        return int(m.group(1)) if m else None

    def uses(self):
        """Operand names this instruction reads."""
        out = []
        for a in self.args:
            n = operand_name(a)
            if n:
                out.append(n)
        return out

    def __repr__(self):
        return '<Instr %s>' % self.body[:60]


class Block(object):
    def __init__(self, label, header_line=None, index=-1):
        self.label = label            # '%bb44' style, or None for the entry
        self.header_line = header_line
        self.index = index            # line number of the label, -1 for entry
        self.instrs = []

    @property
    def terminator(self):
        return self.instrs[-1] if self.instrs else None

    def successors(self):
        t = self.terminator
        if not t:
            return []
        m = _BR_COND.match(t.line)
        if m:
            return [m.group(2), m.group(3)]
        m = _BR_UNCOND.match(t.line)
        if m:
            return [m.group(1)]
        return []

    def find(self, opcode):
        return [i for i in self.instrs if i.dxop == opcode]

    def __repr__(self):
        return '<Block %s, %d instrs>' % (self.label, len(self.instrs))


class Function(object):
    def __init__(self, signature_line, name):
        self.signature_line = signature_line
        self.name = name
        self.blocks = []

    def block(self, label):
        for b in self.blocks:
            if b.label == label:
                return b
        return None

    def instrs(self):
        for b in self.blocks:
            for i in b.instrs:
                yield b, i

    def find(self, opcode):
        """Every dx.op call with this opcode, as (block, instr)."""
        return [(b, i) for b, i in self.instrs() if i.dxop == opcode]

    # --- CFG ---------------------------------------------------------------

    def preds(self):
        p = {b.label: [] for b in self.blocks}
        for b in self.blocks:
            for s in b.successors():
                if s in p:
                    p[s].append(b.label)
        return p

    def dominators(self):
        """Iterative dominator sets, keyed by block label.

        Small CFGs, so the simple fixed-point version is fine and is much
        easier to check by eye than a Lengauer-Tarjan."""
        labels = [b.label for b in self.blocks]
        entry = labels[0]
        preds = self.preds()
        dom = {l: set(labels) for l in labels}
        dom[entry] = {entry}
        changed = True
        while changed:
            changed = False
            for l in labels[1:]:
                ps = [dom[p] for p in preds[l] if p in dom]
                new = set.intersection(*ps) if ps else set()
                new = new | {l}
                if new != dom[l]:
                    dom[l] = new
                    changed = True
        return dom

    def natural_loops(self):
        """Every natural loop, as (header, latch, set_of_body_labels).

        A back edge is latch -> header where header dominates latch. The body
        is the header plus everything that reaches the latch without leaving
        through the header. This is how the rotated `while (q.Proceed())` is
        found: in the IR its guard sits in the preheader and only the latch
        carries the real test, so there is no single 'loop condition' to match
        on textually."""
        dom = self.dominators()
        preds = self.preds()
        loops = []
        for b in self.blocks:
            for succ in b.successors():
                if succ in dom.get(b.label, set()):
                    header, latch = succ, b.label
                    body, stack = {header, latch}, [latch]
                    while stack:
                        cur = stack.pop()
                        for p in preds.get(cur, []):
                            if p not in body:
                                body.add(p)
                                stack.append(p)
                    loops.append((header, latch, body))
        return loops


class Module(object):
    """Header text, functions, and trailing text, all preserved verbatim.

    The transform edits `lines` through the block and instruction objects,
    which hold indices into it, so anything not understood survives."""

    def __init__(self, text):
        self.text = text
        self.functions = []
        self._parse()

    def _parse(self):
        lines = self.text.split('\n')
        self.lines = lines
        fn = None
        blk = None
        for n, line in enumerate(lines):
            if line.startswith('define '):
                m = re.search(r'@([\w.$-]+|"[^"]+")\s*\(', line)
                fn = Function(line, m.group(1).strip('"') if m else '?')
                fn.index = n
                blk = Block(None, None)          # implicit entry block
                fn.blocks.append(blk)
                self.functions.append(fn)
                continue
            if fn is None:
                continue
            if line == '}':
                fn.end_index = n
                fn = blk = None
                continue
            m = re.match(r'^([\w.$-]+):', line)
            if m:
                blk = Block('%' + m.group(1), line, n)
                fn.blocks.append(blk)
                continue
            if line.strip() and not line.strip().startswith(';'):
                blk.instrs.append(Instr(line, n))

    def function(self, name):
        for f in self.functions:
            if f.name == name:
                return f
        return None


def _strip_comment(body):
    """Drop a trailing ; comment, respecting quotes."""
    quote = False
    for i, ch in enumerate(body):
        if ch == '"':
            quote = not quote
        elif ch == ';' and not quote:
            return body[:i].strip()
    return body.strip()


def render(module, edits):
    """Rebuild module text with `edits` applied.

    edits maps a line index to replacement text, or to None to delete that
    line. Anything not mentioned is emitted byte for byte, which is the point:
    the application's own code passes through untouched."""
    out = []
    for n, line in enumerate(module.lines):
        if n in edits:
            rep = edits[n]
            if rep is not None:
                out.append(rep)
        else:
            out.append(line)
    return '\n'.join(out)
