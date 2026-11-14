/* SPDX-License-Identifier: GPL-3.0-or-later
 * ASTC (LDR, 2D) decoded by the graphics card, for cards without ASTC (desktop NVIDIA and AMD, Intel Xe). Before this, every
 * ASTC level was decoded by tex_decode.c on the render thread while the frame waited (2,770 textures, 1 GB of texels in a
 * match). Now the render thread only copies the level's blocks into the stream ring; one compute thread per block decodes it
 * into a scratch buffer, which is copied into the texture. The shader is tex_decode.c's astc_block line for line (the same
 * integer steps, the same handling of absent bits, magenta for an invalid or HDR block), so the texels are the same:
 * STRIKERS_ASTC_VERIFY=1 decodes every level both ways and compares them (test). STRIKERS_ASTC_GPU=0: the processor, as before. */
static const char k_astc_cs[] =
"#version 450\n"
"layout(local_size_x=64) in;\n"
"layout(std430,set=0,binding=0) readonly buffer In { uint src[]; };\n"
"layout(std430,set=0,binding=1) writeonly buffer Out { uint dst[]; };\n"
"layout(push_constant) uniform Pc { uint in_off; uint bx; uint by; uint w; uint h; uint bw; uint bh; uint srgb; } pc;\n"
"uvec4 B; uvec4 RV;\n"
"uint bits_of(uvec4 b,int pos,int n){ if(n<=0||pos>=128)return 0u; int wi=pos>>5,off=pos&31; uint lo=b[wi]>>uint(off);\n"
"  if(off!=0&&wi<3)lo|=b[wi+1]<<uint(32-off); return n>=32?lo:(lo&((1u<<uint(n))-1u)); }\n"
"uint get(bool rev,inout int pos,int end,int n){ int avail=end-pos; uint v=bits_of(rev?RV:B,pos,n<avail?n:avail); pos+=n; return v; }\n"
"const int LV[21]=int[21](2,3,4,5,6,8,10,12,16,20,24,32,40,48,64,80,96,128,160,192,256);\n"
"ivec3 enc(int level){ int t=0,q=0,l=level,b=0; if(level%3==0&&((level/3)&(level/3-1))==0){t=1;l=level/3;} else if(level%5==0&&((level/5)&(level/5-1))==0){q=1;l=level/5;}\n"
"  while(l>1){l>>=1;b++;} return ivec3(b,t,q); }\n"
"int cost(ivec3 e,int n){ return e.x*n+(e.y!=0?(n*8+4)/5:e.z!=0?(n*7+2)/3:0); }\n"
"uint M[64]; uint TQ[64];\n"
"void bise(bool rev,int pos,int end,ivec3 e,int n){ int i=0;\n"
"  if(e.y!=0){ while(i<n){ uint m[5]=uint[5](0u,0u,0u,0u,0u); uint T=0u,C; uint t[5]; int cnt=min(n-i,5);\n"
"      m[0]=get(rev,pos,end,e.x); T=get(rev,pos,end,2);\n"
"      if(cnt>1){m[1]=get(rev,pos,end,e.x); T|=get(rev,pos,end,2)<<2;}\n"
"      if(cnt>2){m[2]=get(rev,pos,end,e.x); T|=get(rev,pos,end,1)<<4;}\n"
"      if(cnt>3){m[3]=get(rev,pos,end,e.x); T|=get(rev,pos,end,2)<<5;}\n"
"      if(cnt>4){m[4]=get(rev,pos,end,e.x); T|=get(rev,pos,end,1)<<7;}\n"
"      if(((T>>2)&7u)==7u){C=(((T>>5)&7u)<<2)|(T&3u);t[4]=2u;t[3]=2u;}\n"
"      else{C=T&0x1Fu; if(((T>>5)&3u)==3u){t[4]=2u;t[3]=(T>>7)&1u;} else{t[4]=(T>>7)&1u;t[3]=(T>>5)&3u;}}\n"
"      if((C&3u)==3u){t[2]=2u;t[1]=(C>>4)&1u;t[0]=(((C>>3)&1u)<<1)|(((C>>2)&1u)&~((C>>3)&1u));}\n"
"      else if(((C>>2)&3u)==3u){t[2]=2u;t[1]=2u;t[0]=C&3u;}\n"
"      else{t[2]=(C>>4)&1u;t[1]=(C>>2)&3u;t[0]=(((C>>1)&1u)<<1)|((C&1u)&~((C>>1)&1u));}\n"
"      for(int k=0;k<cnt;k++){M[i]=m[k];TQ[i]=t[k];i++;} } }\n"
"  else if(e.z!=0){ while(i<n){ uint m[3]=uint[3](0u,0u,0u); uint Q,C; uint q[3]; int cnt=min(n-i,3);\n"
"      m[0]=get(rev,pos,end,e.x); Q=get(rev,pos,end,3);\n"
"      if(cnt>1){m[1]=get(rev,pos,end,e.x); Q|=get(rev,pos,end,2)<<3;}\n"
"      if(cnt>2){m[2]=get(rev,pos,end,e.x); Q|=get(rev,pos,end,2)<<5;}\n"
"      if(((Q>>1)&3u)==3u&&((Q>>5)&3u)==0u){q[0]=4u;q[1]=4u;q[2]=((Q&1u)<<2)|((((Q>>4)&1u)&~(Q&1u))<<1)|(((Q>>3)&1u)&~(Q&1u));}\n"
"      else{ if(((Q>>1)&3u)==3u){q[2]=4u;C=(((Q>>3)&3u)<<3)|(((~(Q>>5))&3u)<<1)|(Q&1u);} else{q[2]=(Q>>5)&3u;C=Q&0x1Fu;}\n"
"        if((C&7u)==5u){q[1]=4u;q[0]=(C>>3)&3u;} else{q[1]=(C>>3)&3u;q[0]=C&7u;} }\n"
"      for(int k=0;k<cnt;k++){M[i]=m[k];TQ[i]=q[k];i++;} } }\n"
"  else{ for(;i<n;i++){M[i]=get(rev,pos,end,e.x);TQ[i]=0u;} } }\n"
"int rep8(int v,int b){ int r=v<<(8-b); if(b==1)return v!=0?255:0; if(b==2)return r|(r>>2)|(r>>4)|(r>>6); if(b==3)return r|(r>>3)|(r>>6); return r|(r>>b); }\n"
"int uq_color(ivec3 e,int m,int tq){ int A=(m&1)!=0?0x1FF:0,Bv=0,C=0,T;\n"
"  if(e.y==0&&e.z==0)return e.x!=0?rep8(m,e.x):0;\n"
"  if(e.y!=0){ if(e.x==1)C=204; else if(e.x==2){int b=(m>>1)&1;C=93;Bv=(b<<8)|(b<<4)|(b<<2)|(b<<1);} else if(e.x==3){int cb=(m>>1)&3;C=44;Bv=(cb<<7)|(cb<<2)|cb;}\n"
"    else if(e.x==4){int d=(m>>1)&7;C=22;Bv=(d<<6)|d;} else if(e.x==5){int d=(m>>1)&15;C=11;Bv=(d<<5)|(d>>2);} else if(e.x==6){int d=(m>>1)&31;C=5;Bv=(d<<4)|(d>>4);} else return 0; }\n"
"  else{ if(e.x==1)C=113; else if(e.x==2){int b=(m>>1)&1;C=54;Bv=(b<<8)|(b<<3)|(b<<2);} else if(e.x==3){int cb=(m>>1)&3;C=26;Bv=(cb<<7)|(cb<<1)|(cb>>1);}\n"
"    else if(e.x==4){int d=(m>>1)&7;C=13;Bv=(d<<6)|(d>>1);} else if(e.x==5){int d=(m>>1)&15;C=6;Bv=(d<<5)|(d>>3);} else return 0; }\n"
"  T=tq*C+Bv; T^=A; T=(A&0x80)|(T>>2); return T; }\n"
"int uq_weight(ivec3 e,int m,int tq){ int A=(m&1)!=0?0x7F:0,Bv=0,C=0,T;\n"
"  if(e.y==0&&e.z==0){ int b=e.x,v=m<<(6-b); if(b==1)T=m!=0?63:0; else if(b==2)T=v|(v>>2)|(v>>4); else if(b==3)T=v|(v>>3); else if(b==4)T=v|(v>>4); else if(b==5)T=v|(v>>5); else T=0; }\n"
"  else if(e.y!=0){ if(e.x==0){T=tq==0?0:tq==1?32:63;return T>32?T+1:T;} if(e.x==1)C=50; else if(e.x==2){int b=(m>>1)&1;C=23;Bv=(b<<6)|(b<<2)|b;} else if(e.x==3){int cb=(m>>1)&3;C=11;Bv=(cb<<5)|cb;} else return 0;\n"
"    T=tq*C+Bv; T^=A; T=(A&0x20)|(T>>2); }\n"
"  else{ if(e.x==0){T=tq==0?0:tq==1?16:tq==2?32:tq==3?47:63;return T>32?T+1:T;} if(e.x==1)C=28; else if(e.x==2){int b=(m>>1)&1;C=13;Bv=(b<<6)|(b<<1);} else return 0;\n"
"    T=tq*C+Bv; T^=A; T=(A&0x20)|(T>>2); }\n"
"  return T>32?T+1:T; }\n"
"uint hash52(uint p){ p^=p>>15; p-=p<<17; p+=p<<7; p+=p<<4; p^=p>>5; p+=p<<16; p^=p>>7; p^=p>>3; p^=p<<6; p^=p>>17; return p; }\n"
"int clamp255(int v){ return v<0?0:v>255?255:v; }\n"
"void bts(inout int a,inout int b){ b>>=1; b|=a&0x80; a>>=1; a&=0x3F; if((a&0x20)!=0)a-=0x40; }\n"
"int CV[18]; ivec4 E0[4]; ivec4 E1[4];\n"
"ivec4 bc(ivec4 e){ e.x=(e.x+e.z)>>1; e.y=(e.y+e.z)>>1; return e; }\n"
"bool endpoints(int cem,int o,int pi){ ivec4 e0,e1; int v0=CV[o],v1=CV[o+1],v2=0,v3=0,v4=0,v5=0,v6=0,v7=0;\n"
"  if(o+2<18)v2=CV[o+2]; if(o+3<18)v3=CV[o+3]; if(o+4<18)v4=CV[o+4]; if(o+5<18)v5=CV[o+5]; if(o+6<18)v6=CV[o+6]; if(o+7<18)v7=CV[o+7];\n"
"  if(cem==0){e0=ivec4(v0,v0,v0,255);e1=ivec4(v1,v1,v1,255);}\n"
"  else if(cem==1){int l0=(v0>>2)|(v1&0xC0),l1=l0+(v1&0x3F); if(l1>255)l1=255; e0=ivec4(l0,l0,l0,255);e1=ivec4(l1,l1,l1,255);}\n"
"  else if(cem==4){e0=ivec4(v0,v0,v0,v2);e1=ivec4(v1,v1,v1,v3);}\n"
"  else if(cem==5){int a0=v0,a1=v1,a2=v2,a3=v3; bts(a1,a0); bts(a3,a2); e0=ivec4(clamp255(a0),clamp255(a0),clamp255(a0),clamp255(a2)); int l=clamp255(a0+a1); e1=ivec4(l,l,l,clamp255(a2+a3));}\n"
"  else if(cem==6){e0=ivec4(v0*v3>>8,v1*v3>>8,v2*v3>>8,255);e1=ivec4(v0,v1,v2,255);}\n"
"  else if(cem==8){ if(v1+v3+v5>=v0+v2+v4){e0=ivec4(v0,v2,v4,255);e1=ivec4(v1,v3,v5,255);} else{e0=bc(ivec4(v1,v3,v5,255));e1=bc(ivec4(v0,v2,v4,255));} }\n"
"  else if(cem==9){int a0=v0,a1=v1,a2=v2,a3=v3,a4=v4,a5=v5; bts(a1,a0); bts(a3,a2); bts(a5,a4);\n"
"    if(a1+a3+a5>=0){e0=ivec4(a0,a2,a4,255);e1=ivec4(a0+a1,a2+a3,a4+a5,255);} else{e0=bc(ivec4(a0+a1,a2+a3,a4+a5,255));e1=bc(ivec4(a0,a2,a4,255));}\n"
"    e0=ivec4(clamp255(e0.x),clamp255(e0.y),clamp255(e0.z),255); e1=ivec4(clamp255(e1.x),clamp255(e1.y),clamp255(e1.z),255); }\n"
"  else if(cem==10){e0=ivec4(v0*v3>>8,v1*v3>>8,v2*v3>>8,v4);e1=ivec4(v0,v1,v2,v5);}\n"
"  else if(cem==12){ if(v1+v3+v5>=v0+v2+v4){e0=ivec4(v0,v2,v4,v6);e1=ivec4(v1,v3,v5,v7);} else{e0=bc(ivec4(v1,v3,v5,v7));e1=bc(ivec4(v0,v2,v4,v6));} }\n"
"  else if(cem==13){int a0=v0,a1=v1,a2=v2,a3=v3,a4=v4,a5=v5,a6=v6,a7=v7; bts(a1,a0); bts(a3,a2); bts(a5,a4); bts(a7,a6);\n"
"    if(a1+a3+a5>=0){e0=ivec4(a0,a2,a4,a6);e1=ivec4(a0+a1,a2+a3,a4+a5,a6+a7);} else{e0=bc(ivec4(a0+a1,a2+a3,a4+a5,a6+a7));e1=bc(ivec4(a0,a2,a4,a6));}\n"
"    e0=ivec4(clamp255(e0.x),clamp255(e0.y),clamp255(e0.z),clamp255(e0.w)); e1=ivec4(clamp255(e1.x),clamp255(e1.y),clamp255(e1.z),clamp255(e1.w)); }\n"
"  else return false;\n"
"  E0[pi]=e0; E1[pi]=e1; return true; }\n"
"void put(int x,int y,uint c){ if(uint(x)<pc.w&&uint(y)<pc.h)dst[uint(y)*pc.w+uint(x)]=c; }\n"
"void fill(int x0,int y0,int bw,int bh,uint c){ for(int y=0;y<bh;y++)for(int x=0;x<bw;x++)put(x0+x,y0+y,c); }\n"
"void main(){ uint id=gl_GlobalInvocationID.x; if(id>=pc.bx*pc.by)return;\n"
"  int bw=int(pc.bw),bh=int(pc.bh),x0=int(id%pc.bx)*bw,y0=int(id/pc.bx)*bh; bool srgb=pc.srgb!=0u;\n"
"  uint o=pc.in_off+id*4u; B=uvec4(src[o],src[o+1u],src[o+2u],src[o+3u]);\n"
"  RV=uvec4(bitfieldReverse(B.w),bitfieldReverse(B.z),bitfieldReverse(B.y),bitfieldReverse(B.x));\n"
"  const uint MAGENTA=0xFFFF00FFu;\n"
"  int pos=0; uint mode=get(false,pos,128,11);\n"
"  if((mode&0x1FFu)==0x1FCu){ bool hdr=((mode>>9)&1u)!=0u;\n"
"    if((mode&0x400u)==0u||get(false,pos,128,1)==0u){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"    get(false,pos,128,13);get(false,pos,128,13);get(false,pos,128,13);get(false,pos,128,13);\n"
"    uint px[4]; for(int i=0;i<4;i++){ uint c=get(false,pos,128,16);\n"
"      if(hdr){float f=unpackHalf2x16(c).x; px[i]=uint(f<=0.0?0:f>=1.0?255:int(f*255.0+0.5));} else px[i]=c>>8; }\n"
"    fill(x0,y0,bw,bh,px[0]|(px[1]<<8)|(px[2]<<16)|(px[3]<<24)); return; }\n"
"  if((mode&0xFu)==0u||((mode&3u)==0u&&(mode&0x1C0u)==0x1C0u)){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  int lay; if((mode&3u)!=0u){ if((mode&8u)!=0u){ if((mode&4u)!=0u)lay=(mode&0x100u)!=0u?4:3; else lay=2; } else lay=(mode&4u)!=0u?1:0; }\n"
"  else{ if((mode&0x100u)!=0u){ if((mode&0x80u)!=0u)lay=(mode&0x20u)!=0u?8:7; else lay=9; } else lay=(mode&0x80u)!=0u?6:5; }\n"
"  int R=int((mode>>4)&1u); if(lay<5)R|=int(mode&3u)<<1; else R|=int(mode&0xCu)>>1;\n"
"  int A=int(mode>>5)&3,Bm=int(mode>>7)&3,gw,gh;\n"
"  if(lay==0){gw=Bm+4;gh=A+2;} else if(lay==1){gw=Bm+8;gh=A+2;} else if(lay==2){gw=A+2;gh=Bm+8;} else if(lay==3){gw=A+2;gh=(Bm&1)+6;}\n"
"  else if(lay==4){gw=(Bm&1)+2;gh=A+2;} else if(lay==5){gw=12;gh=A+2;} else if(lay==6){gw=A+2;gh=12;} else if(lay==7){gw=6;gh=10;} else if(lay==8){gw=10;gh=6;}\n"
"  else{gw=A+6;gh=int((mode>>9)&3u)+6;}\n"
"  bool dual=lay!=9&&(mode&0x400u)!=0u, high=lay!=9&&(mode&0x200u)!=0u;\n"
"  const int lo[6]=int[6](2,3,4,5,6,8); const int hi[6]=int[6](10,12,16,20,24,32);\n"
"  int wlevel=high?hi[R-2]:lo[R-2];\n"
"  if(gw>bw||gh>bh){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  int nweights=gw*gh*(dual?2:1); ivec3 wenc=enc(wlevel); int wbits=cost(wenc,nweights);\n"
"  if(nweights>64||wbits<24||wbits>96){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  int nparts=int(get(false,pos,128,2))+1; if(nparts==4&&dual){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  int cem[4]=int[4](0,0,0,0); int part_index=0,base_cem=0,color_start,extra=0,ccs=0;\n"
"  if(nparts==1){cem[0]=int(get(false,pos,128,4));color_start=17;} else{part_index=int(get(false,pos,128,10));base_cem=int(get(false,pos,128,6));color_start=29;}\n"
"  if(nparts>1&&(base_cem&3)!=0)extra=3*nparts-4;\n"
"  int color_bits=128-wbits-extra-(dual?2:0)-color_start; if(color_bits<0){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  if(nparts>1){ if((base_cem&3)!=0){ int p2=128-wbits-extra; int bits=base_cem|(int(get(false,p2,128,extra))<<6),cls=(base_cem&3)-1; int C[4],Mv[4]; bits>>=2;\n"
"      for(int i=0;i<nparts;i++){C[i]=bits&1;bits>>=1;} for(int i=0;i<nparts;i++){Mv[i]=bits&3;bits>>=2;} for(int i=0;i<nparts;i++)cem[i]=((cls+C[i])<<2)|Mv[i]; }\n"
"    else for(int i=0;i<nparts;i++)cem[i]=base_cem>>2; }\n"
"  if(dual){int p3=128-wbits-extra-2; ccs=int(get(false,p3,128,2));}\n"
"  int nvals=0; for(int i=0;i<nparts;i++)nvals+=((cem[i]>>2)+1)*2; if(nvals>18){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  int clevel=-1; ivec3 cenc=ivec3(0); for(int i=20;i>=4;i--){ivec3 e=enc(LV[i]); if(cost(e,nvals)<=color_bits){clevel=i;cenc=e;break;}}\n"
"  if(clevel<0){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  bise(false,color_start,color_start+color_bits,cenc,nvals);\n"
"  for(int i=0;i<nvals;i++)CV[i]=uq_color(cenc,int(M[i]),int(TQ[i]));\n"
"  bool is_hdr=false; int cidx=0; for(int i=0;i<nparts;i++){ if(!endpoints(cem[i],cidx,i))is_hdr=true; cidx+=((cem[i]>>2)+1)*2; }\n"
"  if(is_hdr){fill(x0,y0,bw,bh,MAGENTA);return;}\n"
"  bise(true,0,wbits,wenc,nweights);\n"
"  int WQ0[64],WQ1[64]; for(int i=0;i<gw*gh;i++){ if(dual){WQ0[i]=uq_weight(wenc,int(M[2*i]),int(TQ[2*i]));WQ1[i]=uq_weight(wenc,int(M[2*i+1]),int(TQ[2*i+1]));} else WQ0[i]=uq_weight(wenc,int(M[i]),int(TQ[i])); }\n"
"  int kx[4]=int[4](0,0,0,0),ky[4]=int[4](0,0,0,0),ad[4]=int[4](0,0,0,0); int shift=bw*bh<31?1:0;\n"
"  if(nparts>1){ int seed=part_index+(nparts-1)*1024; uint rn=hash52(uint(seed)); int s[8]; for(int i=0;i<8;i++){int v=int((rn>>uint(4*i))&15u); s[i]=v*v;}\n"
"    int sh1,sh2; if((seed&1)!=0){sh1=(seed&2)!=0?4:5;sh2=nparts==3?6:5;} else{sh1=nparts==3?6:5;sh2=(seed&2)!=0?4:5;}\n"
"    for(int i=0;i<4;i++){kx[i]=s[2*i]>>sh1;ky[i]=s[2*i+1]>>sh2;} ad[0]=int(rn>>14);ad[1]=int(rn>>10);ad[2]=int(rn>>6);ad[3]=int(rn>>2); }\n"
"  for(int i=0;i<nparts;i++){ E0[i]=srgb?((E0[i]<<8)|0x80):((E0[i]<<8)|E0[i]); E1[i]=srgb?((E1[i]<<8)|0x80):((E1[i]<<8)|E1[i]); }\n"
"  int Ds=(1024+bw/2)/(bw-1),Dt=(1024+bh/2)/(bh-1);\n"
"  for(int y=0;y<bh;y++)for(int x=0;x<bw;x++){\n"
"    int cs=Ds*x,ct=Dt*y,gs=(cs*(gw-1)+32)>>6,gt=(ct*(gh-1)+32)>>6; int js=gs>>4,fs=gs&15,jt=gt>>4,ft=gt&15,w11=(fs*ft+8)>>4,w10=ft-w11,w01=fs-w11,w00=16-fs-ft+w11;\n"
"    int v0=js+jt*gw; int w0,w1;\n"
"    { int p00=WQ0[v0],p01=js+1<gw?WQ0[v0+1]:0,p10=jt+1<gh?WQ0[v0+gw]:0,p11=js+1<gw&&jt+1<gh?WQ0[v0+gw+1]:0; w0=(p00*w00+p01*w01+p10*w10+p11*w11+8)>>4; }\n"
"    w1=w0; if(dual){ int p00=WQ1[v0],p01=js+1<gw?WQ1[v0+1]:0,p10=jt+1<gh?WQ1[v0+gw]:0,p11=js+1<gw&&jt+1<gh?WQ1[v0+gw+1]:0; w1=(p00*w00+p01*w01+p10*w10+p11*w11+8)>>4; }\n"
"    int part=0; if(nparts>1){ int xs=x<<shift,ys=y<<shift; int a=(kx[0]*xs+ky[0]*ys+ad[0])&0x3F,b=(kx[1]*xs+ky[1]*ys+ad[1])&0x3F,c=nparts<3?0:(kx[2]*xs+ky[2]*ys+ad[2])&0x3F,d=nparts<4?0:(kx[3]*xs+ky[3]*ys+ad[3])&0x3F;\n"
"      part=(a>=b&&a>=c&&a>=d)?0:(b>=c&&b>=d)?1:(c>=d)?2:3; }\n"
"    uint outc=0u; for(int c=0;c<4;c++){ int w=dual&&c==ccs?w1:w0; int v=(E0[part][c]*(64-w)+E1[part][c]*w+32)>>6; int o8=srgb?(v>>8):(v==65535?255:(v*255+32768)>>16); outc|=uint(o8&255)<<uint(8*c); }\n"
"    put(x0+x,y0+y,outc); } }\n";

static struct { VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dl; VkDescriptorPool pool; VkDescriptorSet sets[NSLOT]; VkBuffer scratch; VMem scratch_mem;
    uint64_t scratch_size, range; int state, verify; LONG64 levels, blocks, mismatch_levels, mismatch_texels; VkBuffer bound_scratch[NSLOT]; } AG;
extern int nvn_front_astc_gpu;                               /* nvn_gl.c nvn_front_tex_read: hand ASTC levels over as blocks (rgba8 = 2) */

static void astc_gpu_shutdown(void) {
    if(AG.levels)printf("astc-gpu: %lld levels, %lld blocks decoded by the graphics card%s\n",(long long)AG.levels,(long long)AG.blocks,AG.verify?"":"");
    if(AG.verify)printf("astc-gpu: VERIFY: %lld levels compared with the processor's decoder, %lld of them differ (%lld texels)\n",(long long)AG.levels,(long long)AG.mismatch_levels,(long long)AG.mismatch_texels);
    if(AG.pipe)vkDestroyPipeline(V.device,AG.pipe,NULL);
    if(AG.layout)vkDestroyPipelineLayout(V.device,AG.layout,NULL);
    if(AG.pool)vkDestroyDescriptorPool(V.device,AG.pool,NULL);
    if(AG.dl)vkDestroyDescriptorSetLayout(V.device,AG.dl,NULL);
    if(AG.scratch){vkDestroyBuffer(V.device,AG.scratch,NULL);vmem_free(AG.scratch_mem);}
    memset(&AG,0,sizeof AG);nvn_front_astc_gpu=0;
}
/* Called once the device exists: on unless STRIKERS_ASTC_GPU=0 or the card decodes ASTC itself. */
static void astc_gpu_init(int card_has_astc) {
    const char *e=getenv("STRIKERS_ASTC_GPU");
    AG.verify=getenv("STRIKERS_ASTC_VERIFY")&&atoi(getenv("STRIKERS_ASTC_VERIFY"));
    if(card_has_astc || (e && e[0]=='0')) {AG.state=-1;printf("astc-gpu: off (%s)\n",card_has_astc?"the card decodes ASTC itself":"STRIKERS_ASTC_GPU=0");return;}
    VkDescriptorSetLayoutBinding b[2]={{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
    VkDescriptorSetLayoutCreateInfo di={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};di.bindingCount=2;di.pBindings=b;VKC(vkCreateDescriptorSetLayout(V.device,&di,NULL,&AG.dl));
    VkPushConstantRange pr={VK_SHADER_STAGE_COMPUTE_BIT,0,32};
    VkPipelineLayoutCreateInfo li={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&AG.dl;li.pushConstantRangeCount=1;li.pPushConstantRanges=&pr;
    VKC(vkCreatePipelineLayout(V.device,&li,NULL,&AG.layout));
    VkDescriptorPoolSize sz[2]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,NSLOT},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,NSLOT}};
    VkDescriptorPoolCreateInfo pi={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pi.poolSizeCount=2;pi.pPoolSizes=sz;pi.maxSets=NSLOT;VKC(vkCreateDescriptorPool(V.device,&pi,NULL,&AG.pool));
    for(int i=0;i<NSLOT;i++){VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=AG.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&AG.dl;VKC(vkAllocateDescriptorSets(V.device,&ai,&AG.sets[i]));}
    uint64_t hash;VkShaderModule m=module_from_glsl(5,k_astc_cs,"ASTC decode",&hash,NULL);
    if(m) {
        VkComputePipelineCreateInfo ci={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=AG.layout;ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=m;ci.stage.pName="main";
        if(vkCreateComputePipelines(V.device,VK_NULL_HANDLE,1,&ci,NULL,&AG.pipe)!=VK_SUCCESS)AG.pipe=VK_NULL_HANDLE;
        vkDestroyShaderModule(V.device,m,NULL);
    }
    if(!AG.pipe){AG.state=-1;printf("astc-gpu: the decoder could not be made: the processor decodes ASTC\n");return;}
    /* the blocks are read from a window of the stream ring: at most a quarter of it (ring_raw's largest range), within the card's limit */
    uint64_t lim=V.props.limits.maxStorageBufferRange,al=V.props.limits.minStorageBufferOffsetAlignment;
    AG.range=R.ring_size/4+al;if(AG.range>lim)AG.range=lim&~(al-1);if(AG.range>R.ring_size)AG.range=R.ring_size;
    AG.state=1;nvn_front_astc_gpu=1;
    printf("astc-gpu: on (compute decoder; blocks read through a %llu MB window of the stream ring)%s\n",(unsigned long long)(AG.range>>20),AG.verify?"; VERIFY: every level compared with the processor's decoder":"");
}
/* One ASTC level (blocks, deswizzled) of vt: decoded into the scratch buffer by the card, then copied into the image.
 * Returns 0 when it cannot (the caller decodes on the processor). */
static int astc_gpu_level(VTex *vt,Tex *t,int level,int layer,int w,int h,const void *blocks,size_t size) {
    const TexFormat *f=tex_format(t->b.format);
    if(AG.state!=1 || !f || f->codec!=TEX_CODEC_ASTC || vt->rs || vt->is3d)return 0;
    uint32_t bx=(uint32_t)((w+f->bw-1)/f->bw),by=(uint32_t)((h+f->bh-1)/f->bh);
    if((uint64_t)bx*by*16!=size || size>AG.range-V.props.limits.minStorageBufferOffsetAlignment)return 0;
    uint64_t out=(uint64_t)w*h*4;
    if(out>AG.scratch_size) {                                   /* grows: what used the old one has finished first */
        cmd_wait_all();
        if(AG.scratch){vkDestroyBuffer(V.device,AG.scratch,NULL);vmem_free(AG.scratch_mem);}
        AG.scratch_size=out<(16u<<20)?(16u<<20):out*2;
        VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=AG.scratch_size;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateBuffer(V.device,&bi,NULL,&AG.scratch));VkMemoryRequirements rq;vkGetBufferMemoryRequirements(V.device,AG.scratch,&rq);
        AG.scratch_mem=vmem_alloc(rq,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);VKC(vkBindBufferMemory(V.device,AG.scratch,AG.scratch_mem.mem,AG.scratch_mem.off));
        memset(AG.bound_scratch,0,sizeof AG.bound_scratch);
    }
    uint64_t off=ring_raw(blocks,size);                     /* may submit what was recorded so far */
    pass_end();cmd_begin();
    uint64_t al=V.props.limits.minStorageBufferOffsetAlignment,base=off&~(al-1);
    if(base+AG.range>R.ring_size)base=(R.ring_size-AG.range)&~(al-1);
    if(AG.bound_scratch[R.cur]!=AG.scratch) {                 /* this slot's set: the ring window and the scratch buffer (the slot's last use has finished) */
        VkDescriptorBufferInfo bi[2]={{R.ring,0,AG.range},{AG.scratch,0,VK_WHOLE_SIZE}};
        VkWriteDescriptorSet wr[2];memset(wr,0,sizeof wr);
        for(int i=0;i<2;i++){wr[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;wr[i].dstSet=AG.sets[R.cur];wr[i].dstBinding=(uint32_t)i;wr[i].descriptorCount=1;wr[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;wr[i].pBufferInfo=&bi[i];}
        vkUpdateDescriptorSets(V.device,2,wr,0,NULL);AG.bound_scratch[R.cur]=AG.scratch;
    }
    VkMemoryBarrier mb={VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_HOST_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(R.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&mb,0,NULL,0,NULL);
    uint32_t dyn=(uint32_t)base;
    uint32_t pc[8]={(uint32_t)((off-base)/4),bx,by,(uint32_t)w,(uint32_t)h,f->bw,f->bh,f->srgb};
    vkCmdBindPipeline(R.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,AG.pipe);
    vkCmdBindDescriptorSets(R.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,AG.layout,0,1,&AG.sets[R.cur],1,&dyn);
    vkCmdPushConstants(R.cmd,AG.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,32,pc);
    vkCmdDispatch(R.cmd,(bx*by+63)/64,1,1);
    mb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(R.cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,NULL,0,NULL);
    vt_layout(vt,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;memset(&c,0,sizeof c);c.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;c.imageSubresource.mipLevel=(uint32_t)level;
    c.imageSubresource.baseArrayLayer=(uint32_t)layer;c.imageSubresource.layerCount=1;c.imageExtent.width=(uint32_t)w;c.imageExtent.height=(uint32_t)h;c.imageExtent.depth=1;
    if((uint32_t)layer<(uint32_t)vt->layers)vkCmdCopyBufferToImage(R.cmd,AG.scratch,vt->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
    AG.levels++;AG.blocks+=(LONG64)bx*by;R.upload_bytes+=(LONG64)size;
    if(AG.verify) {                                             /* test: the same level by the processor's decoder, texel for texel */
        VMem hm;VkBuffer hb=vbuf_new(out,VK_BUFFER_USAGE_TRANSFER_DST_BIT,&hm);
        VkBufferCopy bc={0,0,out};vkCmdCopyBuffer(R.cmd,AG.scratch,hb,1,&bc);
        cmd_wait_all();
        uint8_t *cpu=malloc((size_t)out);
        if(cpu && hm.map) {
            tex_decode_rgba8(t->b.format,blocks,w,h,cpu);
            long long bad=0;for(uint64_t i=0;i<out;i+=4)if(memcmp(cpu+i,(uint8_t *)hm.map+i,4))bad++;
            if(bad){AG.mismatch_levels++;AG.mismatch_texels+=bad;if(AG.mismatch_levels<=20)printf("astc-gpu: VERIFY: format 0x%x %dx%d level %d: %lld texels differ\n",t->b.format,w,h,level,bad);}
        }
        free(cpu);vkDestroyBuffer(V.device,hb,NULL);vmem_free(hm);
    }
    return 1;
}
