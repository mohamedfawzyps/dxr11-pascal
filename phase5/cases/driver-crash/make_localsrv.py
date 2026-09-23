"""Make the `localsrv` variant of the crashing library.

crash.ll -> localsrv.ll, changing exactly one thing: the record values are
read from a RAW BUFFER at t0, space1 (a local root SRV) instead of the cbuffer
at b0, space1 (local root constants). Everything downstream is untouched, the
same way globalrec was made. The read shape is DXC's own, from
phase5/cases/reference/lib_localsrv_ref.hlsl.

    dxc -dumpbin crash.out.dxil -Fc crash.ll
    python make_localsrv.py crash.ll localsrv.ll
    dxilrt asm localsrv.ll localsrv.out.dxil
"""
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding='utf-8').read()

# The global: a handle, as DXC declares every 6.6 library resource.
old = '@rq_record = external constant %rq_record, align 4\n'
assert s.count(old) == 1
s = s.replace(old, '@rq_recsrv = external constant %dx.types.Handle, align 4\n')
s = s.replace('%rq_record = type { i32, i32 }\n',
              '%rq_record = type { i32, i32 }\n%struct.ByteAddressBuffer = type { i32 }\n', 1)

# Each read: load, createHandleForLib, cbufferLoadLegacy -> load,
# createHandleForLib, annotateHandle, rawBufferLoad.
read = re.compile(
    r'  (%rq\.cbv\w+) = load %rq_record, %rq_record\* @rq_record, align 4\n'
    r'  (%rq\.cbh\w+) = call %dx\.types\.Handle @dx\.op\.createHandleForLib\.rq_record\(i32 160, %rq_record \1\)[^\n]*\n'
    r'  (%rq\.cbr\w+) = call %dx\.types\.CBufRet\.i32 @dx\.op\.cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle \2, i32 0\)[^\n]*\n')

def repl(m):
    v, h, r = m.group(1), m.group(2), m.group(3)
    return ('  %s = load %%dx.types.Handle, %%dx.types.Handle* @rq_recsrv, align 4\n'
            '  %s.lib = call %%dx.types.Handle @dx.op.createHandleForLib.dx.types.Handle(i32 160, %%dx.types.Handle %s)\n'
            '  %s = call %%dx.types.Handle @dx.op.annotateHandle(i32 216, %%dx.types.Handle %s.lib, %%dx.types.ResourceProperties { i32 11, i32 0 })\n'
            '  %s = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %%dx.types.Handle %s, i32 0, i32 undef, i8 3, i32 4)\n'
            % (v, h, v, h, h, r, h))

s, n = read.subn(repl, s)
print('record reads rewritten:', n)
assert n >= 1

# The results change type.
for r in set(re.findall(r'(%rq\.cbr\w+) = call %dx\.types\.ResRet\.i32', s)):
    s = s.replace('extractvalue %%dx.types.CBufRet.i32 %s,' % r,
                  'extractvalue %%dx.types.ResRet.i32 %s,' % r)
assert '@rq_record' not in s.split('!dx.resources')[0].replace('%rq_record* @rq_record', ''), 'a record read survived'

# Declarations: the record's own overload goes; the others must exist.
s = re.sub(r'declare %dx\.types\.Handle @dx\.op\.createHandleForLib\.rq_record\([^\n]*\n', '', s)
for need in ('@dx.op.annotateHandle(', '@dx.op.rawBufferLoad.i32(',
             '@dx.op.createHandleForLib.dx.types.Handle('):
    assert ('declare' in s and need in s), need
if 'call %dx.types.CBufRet.i32 @dx.op.cbufferLoadLegacy.i32(' not in s:
    s = re.sub(r'declare %dx\.types\.CBufRet\.i32 @dx\.op\.cbufferLoadLegacy\.i32\([^\n]*\n', '', s)

# Metadata: drop the CBV record, add an SRV list holding the raw buffer.
m = re.search(r'^(!\d+) = !\{i32 (\d+), %rq_record\* @rq_record, !"rq_record"[^\n]*\n', s, re.M)
assert m
cbv_id = m.group(1)
s = s.replace(m.group(0), '')
res_line = re.search(r'^!dx\.resources = !\{(!\d+)\}', s, re.M).group(1)
tup = re.search(r'^%s = !\{null, null, (!\d+), null\}' % re.escape(res_line), s, re.M)
assert tup, 'resource tuple shape'
cbv_list = tup.group(1)
lst = re.search(r'^%s = !\{([^}]*)\}' % re.escape(cbv_list), s, re.M)
items = [x.strip() for x in lst.group(1).split(',') if x.strip() != cbv_id]
s = s.replace(lst.group(0), '%s = !{%s}' % (cbv_list, ', '.join(items)))
top = max(int(x) for x in re.findall(r'^!(\d+) = ', s, re.M))
srv_list, srv = '!%d' % (top + 1), '!%d' % (top + 2)
s = s.replace(tup.group(0), '%s = !{%s, null, %s, null}' % (res_line, srv_list, cbv_list))
s = s.rstrip('\n') + '\n%s = !{%s}\n%s = !{i32 0, %%struct.ByteAddressBuffer* bitcast (%%dx.types.Handle* @rq_recsrv to %%struct.ByteAddressBuffer*), !"rq_recsrv", i32 1, i32 0, i32 1, i32 11, i32 0, null}\n' % (srv_list, srv, srv)

open(dst, 'w', encoding='utf-8', newline='\n').write(s)
print('wrote', dst)
