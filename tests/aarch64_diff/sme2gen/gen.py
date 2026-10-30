import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
import sys,re,random
sys.path.insert(0,'/tmp/sm')
from sh import enc
random.seed(7)
exec(open(HERE+'/forms.py').read().split("if __name__")[0])
def derive(t,T):
    """returns (value,mask,{placeholder:[bits]}) or None"""
    ws=[]
    for _ in range(40):
        ch={}; s=fill(t,T,ch); r,_e=enc([s])
        if r: ws.append((r[0],ch))
    if not ws: return None
    mask=0xFFFFFFFF; v=ws[0][0]
    for w,_ in ws: mask&=~(w^v)
    base=ws[0][1]; pos={}
    keys=re.findall(r'<([^>]+)>',t)
    br,_=enc([fill(t,T,dict(base))])
    for k in dict.fromkeys(keys):
        n_c=len(PH[k]) if ':' not in k else int(k.split(':')[1].split('-')[1])-int(k.split(':')[1].split('-')[0])+1
        bits=0
        for i in range(n_c):
            c=dict(base); c[k]=i
            r,_=enc([fill(t,T,c)])
            if r: bits|=r[0]^br[0]
        pos.setdefault(k,[]).append(bits)
    return v&mask,mask,pos
