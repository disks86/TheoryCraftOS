import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
import sys,re
exec(open(HERE+'/gen.py').read())
SZ={'b':0,'h':1,'s':2,'d':3}
ents=[]
OPS=['smop4','umop4','sumop4','usmop4','fmop4','bfmop4']
def add(op,sub,srcT,dst,nn,nm):
    mn=op+('s' if sub else 'a')
    t='%s za<t:0-%d>.%s, <n4%d>, <m4%d>'%(mn,{'s':3,'d':7,'h':1}[dst],'%s'.replace('%s',dst),nn,nm)
    # source types use %s -> srcT: build with explicit
    t=t.replace('<n4%d>'%nn,'<n4%d>'%nn)
    r=derive(t.replace('za<t:','za<t:'),srcT)
    if not r: print('NONE',t,srcT,file=sys.stderr); return
    v,m,pos=r
    ents.append((m,v,OPS.index(op),sub,SZ[srcT],SZ[dst],nn,nm,mn))
for sub in (0,1):
    for op in ('smop4','umop4','sumop4','usmop4'):
        for srcT,dst in (('b','s'),('h','s'),('h','d')):
            for nn in (1,2):
                for nm in (1,2): add(op,sub,srcT,dst,nn,nm)
    for srcT,dst in (('h','s'),('s','s'),('d','d'),('h','h')):
        for nn in (1,2):
            for nm in (1,2): add('fmop4',sub,srcT,dst,nn,nm)
    for srcT,dst in (('h','s'),('h','h')):
        for nn in (1,2):
            for nm in (1,2): add('bfmop4',sub,srcT,dst,nn,nm)
with open(''+OUT+'/AArch64Sme2Mop4Forms.inc','w') as f:
    f.write('// Generated from the LLVM encodings of the SME2.1 MOP4 outer products: the fixed bits, the operation, the element sizes and the number of vectors of each operand. Do not edit.\n')
    f.write('enum Sme2Mop4Op : uint8_t {%s};\n'%', '.join('mo_'+o for o in OPS))
    f.write('struct Sme2Mop4Form { uint32_t mask, value; Sme2Mop4Op op; uint8_t subtract, src, dst, nn, nm; };\n')
    f.write('static const Sme2Mop4Form kSme2Mop4Forms[] = {\n')
    for m,v,op,sub,src,dst,nn,nm,mn in ents:
        f.write('    {0x%08x, 0x%08x, mo_%s, %d, %d, %d, %d, %d},  // %s\n'%(m,v,OPS[op],sub,src,dst,nn,nm,mn))
    f.write('};\n')
print(len(ents),'entries')
