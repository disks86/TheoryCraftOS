import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
exec(open(HERE+'/gen2.py').read())
_old=emit
def emit2(mn,srcT,dst,N,shape,scale=1,idxmax=0):
    _old('k_'+mn,mn,srcT,dst,N,shape,scale,idxmax)
# VI shape: single vector indexed
def emit(op,mn,srcT,dst,N,shape,scale,idxmax=0):
    if shape=='VI':
        t='%s za.%s[<w>, <r%d>], <m1v>, z<zi>.%s[<ix:0-%d>]'%(mn,dst,scale,srcT,idxmax)
        r=derive(t,srcT)
        if not r: print('NONE',t,file=sys.stderr); return
        v,m,pos=r
        key='r%d'%scale
        offbits=val_map(t,srcT,key,8 if scale==2 else 4)
        idxbits=val_map(t,srcT,'ix:0-%d'%idxmax,idxmax+1)
        entries.append((m,v,op,1,'VI',SZ[srcT],SZ[dst],[x[0] for x in offbits],scale,[x[0] for x in idxbits],mn))
        return
    _old(op,mn,srcT,dst,N,shape,scale,idxmax)
def E(mn,srcT,dst,N,shape,scale=1,idxmax=0): emit('k_'+mn,mn,srcT,dst,N,shape,scale,idxmax)
for T in 'sd':
    ix=3 if T=='s' else 1
    for N in (2,4):
        for mn in ('add','sub'):
            for sh in 'ASM': E(mn,T,T,N,sh)
        for mn in ('fadd','fsub'): E(mn,T,T,N,'A')
        for mn in ('fmla','fmls'):
            for sh in 'SM': E(mn,T,T,N,sh)
            E(mn,T,T,N,'I',1,ix)
for mn,srcT,dst,ix in [('sdot','h','s',3),('udot','h','s',3),('fdot','h','s',3),('bfdot','h','s',3),('sdot','b','s',3),('udot','b','s',3),('usdot','b','s',3),('sudot','b','s',3),('sdot','h','d',1),('udot','h','d',1)]:
    for N in (2,4):
        for sh in 'SM': E(mn,srcT,dst,N,sh)
        E(mn,srcT,dst,N,'I',1,ix)
for mn,srcT,dst,ix in [('svdot','b','s',3),('uvdot','b','s',3),('suvdot','b','s',3),('usvdot','b','s',3),('svdot','h','d',1),('uvdot','h','d',1),('bfvdot','h','s',3),('fvdot','h','s',3)]:
    for N in (2,4): E(mn,srcT,dst,N,'I',1,ix)
for mn in ('smlal','umlal','smlsl','umlsl','fmlal','fmlsl','bfmlal','bfmlsl'):
    E(mn,'h','s',1,'V',2); E(mn,'h','s',1,'VI',2,7)
    for N in (2,4):
        for sh in 'SM': E(mn,'h','s',N,sh,2)
        E(mn,'h','s',N,'I',2,7)
for mn,srcT,dst,ix in [('smlall','b','s',15),('umlall','b','s',15),('smlsll','b','s',15),('umlsll','b','s',15),('sumlall','b','s',15),('usmlall','b','s',15),('smlall','h','d',7),('umlall','h','d',7),('smlsll','h','d',7),('umlsll','h','d',7)]:
    E(mn,srcT,dst,1,'V',4); E(mn,srcT,dst,1,'VI',4,ix)
    for N in (2,4):
        for sh in 'SM': E(mn,srcT,dst,N,sh,4)
        E(mn,srcT,dst,N,'I',4,ix)
for N in (2,4):
    for mn in ('fadd','fsub','bfadd','bfsub'): E(mn,'h','h',N,'A')
    for mn in ('fmla','fmls','bfmla','bfmls'):
        for sh in 'SM': E(mn,'h','h',N,sh)
        E(mn,'h','h',N,'I',1,7)
for mn in ('svdot','uvdot'): E(mn,'h','s',2,'I',1,3)
ops=sorted({e[2] for e in entries})
SH={'A':0,'S':1,'M':2,'I':3,'V':4,'VI':5}
with open(''+OUT+'/AArch64Sme2Forms.inc','w') as f:
    f.write('// Generated from the LLVM encodings of the SME2 ZA-array instructions (mask / value of the fixed bits, and where the offset and the element index live). Do not edit.\n')
    f.write('enum Sme2Op : uint8_t {'+', '.join(ops)+'};\n')
    f.write('struct Sme2Form { uint32_t mask, value; Sme2Op op; uint8_t group, shape, src, dst, off_scale, off_count, idx_count; uint8_t off_bits[4], idx_bits[4]; };\n')
    f.write('static const Sme2Form kSme2Forms[] = {\n')
    for m,v,op,N,sh,src,dst,ob,sc,ib,mn in entries:
        oc,ic=len(ob),len(ib); ob=(ob+[0]*4)[:4]; ib=(ib+[0]*4)[:4]
        # counts
        f.write('    {0x%08x, 0x%08x, %s, %d, %d, %d, %d, %d, %d, %d, {%s}, {%s}},  // %s\n'%(m,v,op,N,SH[sh],src,dst,sc,oc,ic,','.join(map(str,ob)),','.join(map(str,ib)),mn))
    f.write('};\n')
print(len(entries),'entries')
