"""Build installer/shader-list.json from this PC's test caches. A developer step, not part of setup.

The list says which shader variants and pipeline states the game ends up using, so a new install can
build them from the player's own game files without playing a match. It holds hashes and render state
only, never shader code or SPIR-V: the .spv and .st files of the caches are not read into it.
Fragment shaders hash differently under an internal resolution scale, and that hash is computable,
so every entry seen in one scale class is also listed for the other.
"""
import argparse
import json
import os
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[2]
VERSION=19                                  # MX_VK_VERSION in runtime/gfx/nvn_vk_pipelines.c
STAGE_MAGIC=0x52545356+VERSION
PIPE_MAGIC=0x31455049+VERSION
GS_MAGIC=0x31505347+VERSION
VAR_SIZE=48                                 # VarRec
PIPE_SIZE=152                               # PipeRec
GS_SIZE=160                                 # GsPipeRec
MASK=(1<<64)-1
PRIME=0x100000001B3
INVERSE=pow(PRIME,-1,1<<64)


def fnv(h,data):
    for b in data:h=((h^b)*PRIME)&MASK
    return h


def unfnv(h,data):
    for b in reversed(data):h=((h*INVERSE)&MASK)^b
    return h


VER=struct.pack('<I',VERSION)
def other_class(hb,scaled):
    """The hash of the same fragment code in the other scale class (ssrc_hb)."""
    base=unfnv(hb,VER)
    return fnv(unfnv(base,b'rs2'),VER) if scaled else fnv(fnv(base,b'rs2'),VER)


def variant_hash(hb,key,fo):
    return fnv(fnv(hb,key),fo)


def caches(root):
    for folder,dirs,files in os.walk(root):
        dirs[:]=[d for d in dirs if d not in ('lift','dump','obj','spv','nv','functions','corresponding-source','toolchain','runtime','tools','docs','stage')]
        if 'pipes.bin' in files and (Path(folder)/'stage').is_dir():
            data=(Path(folder)/'pipes.bin').read_bytes()
            if data[:4]==struct.pack('<I',PIPE_MAGIC):yield Path(folder)


def read_cache(folder):
    """vh -> (stage, hb, key bytes, fo bytes, record); only variants of this record version."""
    variants={}
    for path in (folder/'stage').glob('*.var'):
        stage,hb=path.stem.split('_');stage=int(stage);hb=int(hb,16)
        data=path.read_bytes()
        for i in range(0,len(data)-VAR_SIZE+1,VAR_SIZE):
            rec=data[i:i+VAR_SIZE]
            if struct.unpack_from('<I',rec)[0]!=STAGE_MAGIC:continue
            vh=struct.unpack_from('<Q',rec,40)[0]
            variants[(stage,vh)]=(stage,hb,rec[4:20],rec[20:24],rec)
    return variants


def scale_class(folder,vh):
    data=(folder/f'stage/1_{vh:016x}.st').read_bytes()
    if len(data)<100:raise OSError('empty stage record')
    return struct.unpack_from('<I',data,96)[0]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'installer/shader-list.json')
    a=p.parse_args()
    vars_={};pipes=set();gspipes=set();stats=dict(caches=0,real=dict(),derived_seen=0,derived_new=0)
    seen_fragment={}                        # real fragment variant -> class
    mapped={}                               # fragment vh -> vh in the other class
    def add_var(stage,hb,rec):
        vars_.setdefault((stage,hb),{})[rec[4:24]]=rec
    records=[];gs_records=[]
    for folder in caches(ROOT/'build'):
        stats['caches']+=1
        variants=read_cache(folder)
        for (stage,vh),(s,hb,key,fo,rec) in variants.items():
            if s==1:
                try:cls=scale_class(folder,vh)
                except OSError:continue
                seen_fragment[vh]=cls
                other=other_class(hb,cls)
                ovh=variant_hash(other,key,fo)
                mapped[vh]=ovh;mapped[ovh]=vh
                orec=rec[:40]+struct.pack('<Q',ovh)+rec[48:]
                if any(key) or any(fo):add_var(1,other,orec)
            if any(key) or any(fo):add_var(s,hb,rec)
        data=(folder/'pipes.bin').read_bytes()
        for i in range(0,len(data)-PIPE_SIZE+1,PIPE_SIZE):
            rec=data[i:i+PIPE_SIZE]
            if struct.unpack_from('<I',rec)[0]==PIPE_MAGIC:records.append(rec)
        gs=folder/'gspipes.bin'
        data=gs.read_bytes() if gs.exists() else b''
        for i in range(0,len(data)-GS_SIZE+1,GS_SIZE):
            rec=data[i:i+GS_SIZE]
            if struct.unpack_from('<I',rec)[0]==GS_MAGIC:gs_records.append(rec)
    for rec in records:
        pipes.add(rec)
        fs=struct.unpack_from('<Q',rec,16)[0]
        if fs and fs in mapped:pipes.add(rec[:16]+struct.pack('<Q',mapped[fs])+rec[24:])
    for rec in gs_records:
        gspipes.add(rec)
        fs=struct.unpack_from('<Q',rec,24)[0]
        if fs and fs in mapped:gspipes.add(rec[:24]+struct.pack('<Q',mapped[fs])+rec[32:])
    real0={v for v,c in seen_fragment.items() if c==0};real1={v for v,c in seen_fragment.items() if c==1}
    predicted={mapped[v] for v in real0 if v in mapped}
    stats['real']=dict(fragment_class0=len(real0),fragment_class1=len(real1),class1_predicted_from_class0=len(predicted),
                       class1_real_and_predicted=len(predicted&real1),class1_real_not_predicted=len(real1-predicted))
    out=dict(version=VERSION,note='Hashes and render state only. See tools/release/make_shader_list.py.',
             vars={f'{s}_{hb:016x}':b''.join(list(r.values())[:32]).hex() for (s,hb),r in sorted(vars_.items())},
             pipes=sorted(r.hex() for r in pipes),gspipes=sorted(r.hex() for r in gspipes))
    a.out.write_text(json.dumps(out,separators=(',',':')))
    print(json.dumps(dict(stats,var_files=len(out['vars']),pipes=len(out['pipes']),gspipes=len(gspipes),bytes=a.out.stat().st_size),indent=1))


if __name__=='__main__':main()
