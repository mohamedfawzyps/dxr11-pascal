"""One-variable test: annotate the record handle, change nothing else.

Takes a library lowered by the 0.40.0 rewriter, whose record is a raw buffer
read through the UNANNOTATED form (a %rq_record global, createHandleForLib on
it, rawBufferLoad), and rewrites only the handle into DXC's Shader Model 6.6
form: a %dx.types.Handle global, createHandleForLib on that, then
annotateHandle as a raw buffer. The resource record bitcasts, as DXC's does.

    dxc -dumpbin in.out.dxil -Fc in.ll
    python make_annotated.py in.ll out.ll [props]
    dxilrt asm out.ll out.out.dxil

`props` is the ResourceProperties body, "i32 11, i32 0" (a raw buffer) by
default. "i32 13, i32 8" annotates the OLD cbuffer record instead, which is
how the original crash was re-tested with the annotation as its only change.
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
props = sys.argv[3] if len(sys.argv) > 3 else 'i32 11, i32 0'
s = open(src, encoding='utf-8').read()

old = '@rq_record = external constant %rq_record, align 4\n'
assert s.count(old) == 1
s = s.replace(old, '@rq_record = external constant %dx.types.Handle, align 4\n')

pat = re.compile(
    r'  (%rq\.cbv\w+) = load %rq_record, %rq_record\* @rq_record, align 4\n'
    r'  (%rq\.cbh\w+) = call %dx\.types\.Handle @dx\.op\.createHandleForLib\.rq_record\(i32 160, %rq_record \1\)[^\n]*\n')

def repl(m):
    v, h = m.group(1), m.group(2)
    return ('  %s = load %%dx.types.Handle, %%dx.types.Handle* @rq_record, align 4\n'
            '  %s.lib = call %%dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %%dx.types.Handle %s)  ; CreateHandleForLib(Resource)\n'
            '  %s = call %%dx.types.Handle @dx.op.annotateHandle(i32 216, %%dx.types.Handle %s.lib, %%dx.types.ResourceProperties { %s })  ; AnnotateHandle(res,props)\n'
            % (v, h, v, h, h, props))

s, n = pat.subn(repl, s)
print('record handles annotated:', n)
assert n >= 1

s = re.sub(r'declare %dx\.types\.Handle @dx\.op\.createHandleForLib\.rq_record\([^\n]*\n', '', s)
for need in ('@dx.op.annotateHandle(', '@dx.op.createHandleForLib.dx.types.Handle('):
    assert ('declare %dx.types.Handle ' + need) in s, 'missing declaration ' + need

old = '%rq_record* @rq_record, !"rq_record"'
assert s.count(old) == 1
s = s.replace(old, '%rq_record* bitcast (%dx.types.Handle* @rq_record to %rq_record*), !"rq_record"')

open(dst, 'w', encoding='utf-8', newline='\n').write(s)
print('wrote', dst)
