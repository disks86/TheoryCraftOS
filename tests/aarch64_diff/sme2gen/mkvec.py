import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
import sys,re
exec(open(HERE+'/gen.py').read())
ents=[]
def sve_word(line):
    r,_=enc([line]); return r[0] if r else None
def E(mn,T,N,shape,mode,svemn=None,sve_asm=None):
    zn={2:'<n2>',4:'<n4>'}[N]
    if shape=='S': t='%s %s, %s, <m1w>'%(mn,zn,zn)
    elif shape=='M': t='%s %s, %s, <m%d>'%(mn,zn,zn,N)
    elif shape=='C': t='%s %s, <m1w>, <m1v>'%(mn,zn)
    r=derive(t,T)
    if not r: print('NONE',t,T,file=sys.stderr); return
    v,m,pos=r
    sv=svemn or mn
    if mode==0: w=sve_word('%s z0.%s, p7/m, z0.%s, z0.%s'%(sv,T,T,T))
    elif mode in (1,3): w=sve_word('%s z0.%s, z0.%s, z0.%s'%(sv,T,T,T))
    else: w=sve_word('%s z0.%s, z0.%s, z0.%s'%(sv,T,T,T))
    if w is None: print('NOSVE',sv,T,file=sys.stderr); return
    ents.append((m,v,N,{'S':0,'M':1,'C':2}[shape],mode,w,mn+'.'+T))
for N in (2,4):
    for T in 'bhsd':
        for mn in ('smax','smin','umax','umin'):
            E(mn,T,N,'S',0); E(mn,T,N,'M',0)
        E('add',T,N,'S',0)
        for mn in ('srshl','urshl'): E(mn,T,N,'S',0); E(mn,T,N,'M',0)
        E('sqdmulh',T,N,'S',1); E('sqdmulh',T,N,'M',1)
        E('sclamp',T,N,'C',2); E('uclamp',T,N,'C',2)
    for T in 'hsd':
        for mn in ('fmax','fmin','fmaxnm','fminnm','fscale','famax','famin'):
            E(mn,T,N,'S',0); E(mn,T,N,'M',0)
        E('fclamp',T,N,'C',2)
    for mn in ('bfmax','bfmin','bfmaxnm','bfminnm'):
        E(mn,'h',N,'S',0); E(mn,'h',N,'M',0)
    E('bfclamp','h',N,'C',2)
    for T in 'hsd':
        E('fmul',T,N,'S',3); E('fmul',T,N,'M',3)
    E('bfmul','h',N,'S',3); E('bfmul','h',N,'M',3)
with open(''+OUT+'/AArch64Sme2VecForms.inc','w') as f:
    f.write('// Generated from the LLVM encodings of the SME2 multi-vector arithmetic: the fixed bits, the shape, and the equivalent single-vector SVE instruction word. Do not edit.\n')
    f.write('struct Sme2VecForm { uint32_t mask, value; uint8_t group, shape, mode; uint32_t sve_word; };\n')
    f.write('static const Sme2VecForm kSme2VecForms[] = {\n')
    for m,v,N,sh,mode,w,name in ents:
        f.write('    {0x%08x, 0x%08x, %d, %d, %d, 0x%08x},  // %s\n'%(m,v,N,sh,mode,w,name))
    f.write('};\n')
print(len(ents),'entries')
