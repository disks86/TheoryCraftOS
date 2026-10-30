import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
import sys,re
exec(open(HERE+'/gen.py').read())
SZ={'b':0,'h':1,'s':2,'d':3,'q':4}
ents=[]
KINDS=['ZIP','UZP','UNPK','FCVTW','FCVTN','NARROW','RSHR','SVEMAP','SEL']
def add(kind,t,T,a=0,b=0,c=0,d=0,sve=0,name=''):
    r=derive(t,T)
    if not r: print('NONE',t,T,file=sys.stderr); return
    v,m,pos=r
    ents.append((m,v,kind,a,b,c,d,sve,name))
def sw(line):
    r,_=enc([line]); return r[0] if r else 0
for T in 'bhsdq':
    add('ZIP','zip <n2>, <m1v>, <m1w>',T,SZ[T],2,name='zip2.'+T)
    add('ZIP','zip <n4>, <m4>',T,SZ[T],4,name='zip4.'+T)
    add('UZP','uzp <n2>, <m1v>, <m1w>',T,SZ[T],2,name='uzp2.'+T)
    add('UZP','uzp <n4>, <m4>',T,SZ[T],4,name='uzp4.'+T)
for mn,sg in (('sunpk',1),('uunpk',0)):
    for T in ('h/b','s/h','d/s'):
        d,s=T.split('/')
        add('UNPK','%s <n2>, <m1u>'%mn,T,SZ[d],SZ[s],sg,2,name=mn+T)
        add('UNPK','%s <n4>, <m2u>'%mn,T,SZ[d],SZ[s],sg,4,name=mn+T)
add('FCVTW','fcvt <n2>, <m1u>','s/h',name='fcvt.widen')
for mn,bf,il in (('fcvt',0,0),('fcvtn',0,1),('bfcvt',1,0),('bfcvtn',1,1)):
    add('FCVTN','%s <m1v>, <m2u>'%mn,'h/s',bf,il,name=mn)
# narrowing: kind NARROW a=mode (0 sq,1 uq,2 squ) b=dst esz c=nsrc d=interleave | esz src derived as b+1 (x2) or +? stored separately in sve field
def nar(mn,mode,T,nsrc,il):
    d,s=T.split('/')
    add('NARROW','%s <m1v>, <m%du>'%(mn,nsrc),T,mode,SZ[d],nsrc,il,sve=SZ[s],name=mn+T)
for mn,mode,il in (('sqcvt',0,0),('uqcvt',1,0),('sqcvtu',2,0),('sqcvtn',0,1),('uqcvtn',1,1),('sqcvtun',2,1)):
    for T,n in (('h/s',2),('b/s',4),('h/d',4)): nar(mn,mode,T,n,il)
def rsh(mn,mode,T,nsrc,il):
    d,s=T.split('/')
    mx={('h','s'):16,('b','s'):32,('h','d'):64}[(d,s)]
    add('RSHR','%s <m1v>, <m%du>, #<o:1-%d>'%(mn,nsrc,mx),T,mode,SZ[d],nsrc,il,sve=SZ[s],name=mn+T)
for mn,mode,il in (('sqrshr',0,0),('uqrshr',1,0),('sqrshru',2,0),('sqrshrn',0,1),('uqrshrn',1,1),('sqrshrun',2,1)):
    for T,n in (('h/s',2),('b/s',4),('h/d',4)): rsh(mn,mode,T,n,il)
for mn in ('frintn','frintm','frintp','frinta'):
    for N in (2,4):
        add('SVEMAP','%s <n%d>, <m%d>'%(mn,N,N),'s',N,sve=sw('%s z0.s, p7/m, z0.s'%mn),name=mn)
for mn in ('scvtf','ucvtf','fcvtzs','fcvtzu'):
    for N in (2,4):
        add('SVEMAP','%s <n%d>, <m%d>'%(mn,N,N),'s',N,sve=sw('%s z0.s, p7/m, z0.s'%mn),name=mn)
for T in 'bhsd':
    add('SEL','sel <n2>, <p>, <n2>, <m2>',T,SZ[T],2,name='sel2'+T)
    add('SEL','sel <n4>, <p>, <n4>, <m4>',T,SZ[T],4,name='sel4'+T)
with open(''+OUT+'/AArch64Sme2MiscForms.inc','w') as f:
    f.write('// Generated from the LLVM encodings of the SME2 multi-vector permutes, conversions and selects: the fixed bits, the kind and its parameters. Do not edit.\n')
    f.write('enum Sme2MiscKind : uint8_t {%s};\n'%', '.join('km_'+k for k in KINDS))
    f.write('struct Sme2MiscForm { uint32_t mask, value; Sme2MiscKind kind; uint8_t a, b, c, d; uint32_t sve_word; };\n')
    f.write('static const Sme2MiscForm kSme2MiscForms[] = {\n')
    for m,v,k,a,b,c,d,sv,name in ents:
        f.write('    {0x%08x, 0x%08x, km_%s, %d, %d, %d, %d, 0x%08x},  // %s\n'%(m,v,k,a,b,c,d,sv,name))
    f.write('};\n')
print(len(ents),'entries')
