import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
import itertools,random,re,sys
sys.path.insert(0,'/tmp/sm')
from sh import enc
random.seed(1)
# placeholder candidates
PH={
 'w':['w8','w9','w10','w11'],
 'o':['0','1','2','3'],          # overridden per template through 'ranges'
 'n2':['{z0.%s, z1.%s}','{z2.%s, z3.%s}','{z16.%s, z17.%s}','{z30.%s, z31.%s}'],
 'n4':['{z0.%s-z3.%s}','{z4.%s-z7.%s}','{z16.%s-z19.%s}','{z28.%s-z31.%s}'],
 'm1':['z0.%s','z1.%s','z7.%s','z15.%s','z16.%s','z31.%s'],
 'm1x':['z16.%s','z20.%s','z31.%s'],
 'm2':['{z0.%s, z1.%s}','{z2.%s, z3.%s}','{z8.%s, z9.%s}','{z30.%s, z31.%s}'],
 'm4':['{z0.%s-z3.%s}','{z4.%s-z7.%s}','{z8.%s-z11.%s}','{z28.%s-z31.%s}'],
 'p':['pn8','pn9','pn15'],
 'zi':['0','5','15'],
 'vn2':['v1','v7','v31'],
 'zn2':['z1','z7','z31'],
 'zd':['z0','z9','z31'],
 'zn':['z0','z6','z31'],
 'zm':['z0','z7','z31'],
 'zk':['z0','z7','z7'],
 'zk4':['z0','z7','z15'],
 'zp':['{z0.%s, z1.%s}','{z2.%s, z3.%s}','{z30.%s, z31.%s}'],
 'vd':['v0','v9','v31'],
 'vn':['v0','v6','v31'],
 'vm':['v0','v7','v31'],
 'vk':['v0','v7','v15'],
 'n41':['z0.%s','z2.%s','z14.%s'],
 'n42':['{z0.%s, z1.%s}','{z2.%s, z3.%s}','{z14.%s, z15.%s}'],
 'm41':['z16.%s','z18.%s','z30.%s'],
 'm42':['{z16.%s, z17.%s}','{z18.%s, z19.%s}','{z30.%s, z31.%s}'],
 'n2u':['{z0.%t, z1.%t}','{z2.%t, z3.%t}','{z16.%t, z17.%t}','{z30.%t, z31.%t}'],
 'n4u':['{z0.%t-z3.%t}','{z4.%t-z7.%t}','{z16.%t-z19.%t}','{z28.%t-z31.%t}'],
 'm1u':['z0.%t','z5.%t','z31.%t'],
 'm2u':['{z0.%t, z1.%t}','{z2.%t, z3.%t}','{z8.%t, z9.%t}','{z30.%t, z31.%t}'],
 'm4u':['{z0.%t-z3.%t}','{z4.%t-z7.%t}','{z8.%t-z11.%t}','{z28.%t-z31.%t}'],
 'r2':['0:1','2:3','4:5','6:7','8:9','10:11','12:13','14:15'],
 'r4':['0:3','4:7','8:11','12:15'],
 'm1v':['z0.%s','z5.%s','z31.%s'],
 'm1w':['z0.%s','z7.%s','z15.%s','z16.%s','z31.%s'],
}
def fill(t,T,choice):
    # t contains tokens like <w> <o:0-7> <n2> ...
    U=T.split('/')[1] if '/' in T else T
    T=T.split('/')[0]
    def rep(m):
        k=m.group(1)
        if ':' in k:
            name,rng=k.split(':'); a,b=map(int,rng.split('-')); vals=[str(x) for x in range(a,b+1)]
            return vals[choice.setdefault(k,random.randrange(len(vals)))]
        vals=PH[k]
        v=vals[choice.setdefault(k,random.randrange(len(vals)))]
        return v.replace('%s',T).replace('%t',U)
    return re.sub(r'<([^>]+)>',rep,t).replace('%s',T).replace('%t',U)
def analyze(t,T='s',n=24):
    keys=re.findall(r'<([^>]+)>',t)
    ws=[]
    for _ in range(n):
        ch={}
        s=fill(t,T,ch)
        r,_e=enc([s])
        if r: ws.append((r[0],s,ch))
    if not ws: print('NONE',t); return
    mask=0xFFFFFFFF; v=ws[0][0]
    for w,_,_ in ws: mask&=~(w^v)
    pat=''.join(('1' if (v>>b)&1 else '0') if (mask>>b)&1 else 'x' for b in range(31,-1,-1))
    print(f'{v&mask:08x}/{mask:08x} {pat}  {t.replace("%s",T)}')
    # per-placeholder bit positions: vary single placeholder from first sample
    base_ch=ws[0][2]
    for k in dict.fromkeys(keys):
        bits=0
        for w,s,ch in ws:
            if ch.get(k)!=base_ch.get(k):
                pass
        # vary k only
        base_s=fill(t,T,dict(base_ch)); br,_=enc([base_s])
        n_c=(len(PH[k]) if ':' not in k else int(k.split(':')[1].split('-')[1])-int(k.split(':')[1].split('-')[0])+1)
        for i in range(n_c):
            c=dict(base_ch); c[k]=i
            r,_=enc([fill(t,T,c)])
            if r and br: bits|=r[0]^br[0]
        print('   %-6s bits: %s'%(k.split(':')[0],','.join(str(b) for b in range(31,-1,-1) if (bits>>b)&1)))
if __name__=='__main__':
    T=sys.argv[1]
    for l in sys.stdin:
        l=l.strip()
        if l: analyze(l,T)
