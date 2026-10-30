import os
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '../../../src/Emulation'))
import subprocess,re,sys
def enc(lines):
    r=subprocess.run(['llvm-mc','--triple=aarch64','--mattr=+all','--show-encoding'],input=('\n'.join(lines)+'\n').encode(),capture_output=True)
    out=re.findall(rb'encoding: \[(0x[0-9a-f]{2}),(0x[0-9a-f]{2}),(0x[0-9a-f]{2}),(0x[0-9a-f]{2})\]',r.stdout)
    res=[int(b[0],16)|int(b[1],16)<<8|int(b[2],16)<<16|int(b[3],16)<<24 for b in out]
    return res,r.stderr.decode()
def sheet(lines):
    # lines: asm; first is base. print each in binary with changed bits marked
    ws=[]
    for l in lines:
        r,e=enc([l])
        ws.append(r[0] if r else None)
    base=ws[0]
    for l,w in zip(lines,ws):
        if w is None: print('ERR ',l); continue
        d=w^base
        print('%08x %s  diff=%s  %s'%(w,format(w,'032b'),format(d,'032b').replace('0','.').replace('1','^'),l))
if __name__=='__main__':
    sheet([l.strip() for l in sys.stdin if l.strip()])
