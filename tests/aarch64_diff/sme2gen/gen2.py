import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
import sys,re
exec(open(HERE+'/gen.py').read())
SZ={'b':0,'h':1,'s':2,'d':3}
entries=[]
def val_map(t,T,key,maxv):
    keys=re.findall(r'<([^>]+)>',t)
    base={k:0 for k in keys}
    br,_=enc([fill(t,T,dict(base))])
    out=[]; j=1; b=0
    while j<maxv:
        c=dict(base); c[key]=j
        r,_=enc([fill(t,T,c)])
        if not r: break
        d=r[0]^br[0]; bits=[x for x in range(32) if (d>>x)&1]
        out.append(bits); j<<=1
    return out
def emit(op,mn,srcT,dst,N,shape,scale,idxmax=0):
    # template
    w='<w>'
    off=('<r%d>'%scale) if scale>1 else '<o:0-7>'
    vg=', vgx%d'%N if N>1 else ''
    A='za.%s[%s, %s%s]'%(dst,w,off,vg)
    n={1:'<m1v>',2:'<n2>',4:'<n4>'}[N]
    if shape=='A': t='%s %s, %s'%(mn,A,n)
    elif shape=='S': t='%s %s, %s, <m1>'%(mn,A,n)
    elif shape=='M': t='%s %s, %s, <m%d>'%(mn,A,n,N)
    elif shape=='V': t='%s %s, %s, <m1w>'%(mn,A,n)
    elif shape=='I': t='%s %s, %s, z<zi>.%s[<ix:0-%d>]'%(mn,A,n,'%s',idxmax)
    t=t.replace('.%s[<ix','.'+srcT+'[<ix').replace('za.%s','za.%s')
    # types: sources are srcT, but the template uses %s for T; we substitute srcT for sources and dst for ZA manually
    # make all vector arrangements use srcT via the %s mechanism: fill() replaces %s with T=srcT; za dst literal already substituted
    r=derive(t,srcT)
    if not r: print('NONE',mn,srcT,dst,N,shape,file=sys.stderr); return
    v,m,pos=r
    # off bits
    if scale>1:
        key='r%d'%scale; offmax=8 if scale==2 else 4
    else:
        key='o:0-7'; offmax=8
    offbits=val_map(t,srcT,key,offmax) if True else []
    idxbits=val_map(t,srcT,'ix:0-%d'%idxmax,idxmax+1) if shape=='I' else []
    for L in offbits+idxbits:
        assert len(L)==1,(t,L)
    entries.append((m,v,op,N,shape,SZ[srcT],SZ[dst],[x[0] for x in offbits],scale,[x[0] for x in idxbits],mn))
