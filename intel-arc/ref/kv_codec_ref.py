import ctypes, struct, sys, numpy as np
f32=np.float32
B=open('/data/yasakani/model-speed/kvq_u5.bin','rb').read()
u16=lambda o:struct.unpack_from('<H',B,o)[0]
u32=lambda o:struct.unpack_from('<I',B,o)[0]
NM=u16(24); OFF_M=u32(32); OFF_T=u32(36); OFF_G=u32(40)
MULT=np.frombuffer(B,'<f4',NM,OFF_M)
def group(g):
    go=u32(OFF_G+4*g); mode,kk,tix,guard,m,has_mu=B[go:go+6]
    mu=np.frombuffer(B,'<f2',256,go+8).astype(f32) if has_mu else None
    U=[np.frombuffer(B,'<f2',256,go+8+(512 if has_mu else 0)+512*j).astype(f32) for j in range(m)]
    to=u32(OFF_T+4*tix); gain=np.frombuffer(B,'<f4',1,to+4)[0]
    ci=np.frombuffer(B,'<i2',256,to+12).astype(np.int64)
    return dict(mode=mode,kk=kk,guard=guard,m=m,mu=mu,U=U,gain=gain,ci=ci)
def tree32(v):
    v=v.astype(f32).copy()
    for o in (16,8,4,2,1):
        v=(v+v[np.arange(32)^o]).astype(f32)
    return v[0]
def S(v):                       # 256 -> scalar: per-32 tree, then sequential over the 8 chunks
    t=f32(0)
    for w in range(8): t=f32(t+tree32(v[32*w:32*w+32]))
    return t
def f16r(x): return f32(np.float16(x))
def sign(d):
    x=(d+1)&0xffffffff
    x^=x>>16; x=(x*0x7feb352d)&0xffffffff; x^=x>>15; x=(x*0x846ca68b)&0xffffffff; x^=x>>16
    return f32(-1.0) if x&1 else f32(1.0)
SG=np.array([sign(d) for d in range(256)],f32)
def rotate(x,inverse=False):
    b=(x if inverse else x*SG).astype(f32); idx=np.arange(256)
    for h in (1,2,4,8,16,32,64,128):
        a=b; c=b[idx^h]
        b=np.where(idx&h, c-a, a+c).astype(f32)
    y=(b*f32(0.0625)).astype(f32)
    return (y*SG).astype(f32) if inverse else y
def store8(x):
    mx=np.max(np.abs(x)); sc=f32(mx/f32(127))
    q=np.rint(x/sc) if sc>0 else np.zeros(256)
    return np.clip(q,-127,127).astype(np.int8), sc
def viterbi(xi,ci,kk=5):
    UU=256>>kk; KQ=1<<kk; SH=8-kk
    cost=np.full(256,0x40000000,np.int64); cost[0]=0
    bp=np.zeros((64,UU),np.int64); u=np.arange(256)>>kk
    for t in range(64):
        c2=cost.reshape(KQ,UU)             # [j][u] = cost[j*UU+u]
        jb=np.argmin(c2,axis=0); mn=c2[jb,np.arange(UU)]
        bp[t]=jb; cost=(xi[t]-ci)**2+mn[u]
    st=int(np.argmin(cost)); out=np.zeros(64,np.int64)
    for t in range(63,-1,-1):
        out[t]=st; st=(int(bp[t][st>>kk])<<SH)|(st>>kk)
    return out
def encode(y,g,pos=0):
    P=group(g)
    if pos<P['guard']: return None
    r=y.astype(f32)
    if P['mu'] is not None: r=(r-P['mu']).astype(f32)
    cj=[]
    for U in P['U']:
        c=f16r(S(r*U)); cj.append(c); r=(r-c*U).astype(f32)
    rms=np.sqrt(f32(S(r*r)/f32(256)),dtype=f32)
    if rms==0: rms=f32(1)
    cdec=((P['ci'].astype(f32)*f32(0.00390625))*P['gain']).astype(f32)
    best=None
    for mi in range(NM):
        s=f16r(f32(rms*MULT[mi]))
        if s==0: s=f32(1)
        xi=np.clip(np.rint((r/s).astype(f32)*f32(256)),-2048,2048).astype(np.int64)
        st=np.concatenate([viterbi(xi[64*c:64*c+64],P['ci'],P['kk']) for c in range(4)])
        rec=(cdec[st]*s).astype(f32); dd=(r-rec).astype(f32)
        e=S(dd*dd)
        if best is None or e<best[0]: best=(e,mi,s,st,rec)
    e,mi,s,st,rec=best
    x=rec
    for c,U in zip(cj,P['U']): x=(x+c*U).astype(f32)
    if P['mu'] is not None: x=(x+P['mu']).astype(f32)
    kb=P['kk']; fld=st&((1<<kb)-1)
    bits=np.zeros(256*kb,np.uint8)
    for e_ in range(256):
        for jb in range(kb): bits[e_*kb+jb]=(fld[e_]>>(kb-1-jb))&1
    words=np.packbits(bits).view('>u4').astype('<u4')
    return dict(words=words,s=s,st=st,x=x,mi=mi,cj=cj)
def decode_states(words,kb=5):
    bits=np.unpackbits(words.astype('>u4').view(np.uint8)); st=np.zeros(256,np.int64)
    for d in range(256):
        endb=kb*(d+1); lo=max(endb-8,kb*(d&~63)); v=0
        for b in bits[lo:endb]: v=(v<<1)|int(b)
        st[d]=v
    return st
if __name__=='__main__':
    lib=ctypes.CDLL('/home/shi3z/yasakani-src/yasakani/build/libyskn.so')
    rng=np.random.default_rng(1)
    groups=[0,1,3,5,82,83,96+2,106,117,119,121,123,125,127]*2
    rows=len(groups); x=np.zeros((rows,256),f32)
    for i in range(rows):
        x[i]=rng.standard_normal(256).astype(f32)*f32(rng.uniform(0.05,3.0))
        if i%3==0: x[i]=x[i]**3      # heavy tails
        if i%5==0: x[i,7]=40*np.abs(x[i]).max()
    grp=np.array(groups,np.int32); pos=np.arange(rows,dtype=np.int32)+2
    # --- probe 1: rotation + int8 store
    q=np.zeros((rows,256),np.int8); sc=np.zeros(rows,f32); y=np.zeros((rows,256),f32)
    P=ctypes.c_void_p
    rc=lib.yskn_kvx_probe(x.ctypes.data_as(P),q.ctypes.data_as(P),sc.ctypes.data_as(P),y.ctypes.data_as(P),rows); print('kvx_probe rc',rc,'codec',lib.yskn_kvx_codec())
    ok=True
    for i in range(rows):
        v=rotate(x[i]); q8,s=store8(v); yy=rotate((q8.astype(f32)*s).astype(f32),True)
        ok&=np.array_equal(q8,q[i]) and s==sc[i] and np.array_equal(yy,y[i])
    print('rotation+int8 bit-exact:',ok)
    # --- probe 2: pack/unpack
    stride=160
    yfq=np.zeros((rows,256),f32); pay=np.zeros((rows,stride),np.uint8); side=np.zeros((rows,2),np.uint32); ydec=np.zeros((rows,256),f32); ms=np.zeros(2,f32)
    lib.yskn_tbp_pack_unpack.argtypes=[P,ctypes.c_size_t,P,P,P,ctypes.c_int,ctypes.c_int,P,P,P,P,P]
    rc=lib.yskn_tbp_pack_unpack(B,len(B),x.ctypes.data_as(P),grp.ctypes.data_as(P),pos.ctypes.data_as(P),rows,stride,yfq.ctypes.data_as(P),pay.ctypes.data_as(P),side.ctypes.data_as(P),ydec.ctypes.data_as(P),ms.ctypes.data_as(P)); print('pack_unpack rc',rc)
    for i in range(rows):
        E=encode(x[i],int(grp[i]),int(pos[i])); w=pay[i].view('<u4')
        sref=np.frombuffer(side[i,:1].tobytes(),'<f4')[0]
        print(i,'g',grp[i],'payload',np.array_equal(w,E['words']),'scale',E['s']==sref,'mi',E['mi'],'y_fq',np.array_equal(E['x'],yfq[i]),'y_dec==y_fq',np.array_equal(ydec[i],yfq[i]),'states rt',np.array_equal(decode_states(w),E['st']))
    # probe 3: slow reference kernel
    y3=np.zeros((rows,256),f32)
    lib.yskn_tb_probe.argtypes=[P,ctypes.c_size_t,P,P,P,P,ctypes.c_int]
    rc=lib.yskn_tb_probe(B,len(B),x.ctypes.data_as(P),grp.ctypes.data_as(P),pos.ctypes.data_as(P),y3.ctypes.data_as(P),rows); print('tb_probe rc',rc,'slow==fast',np.array_equal(y3,yfq))
