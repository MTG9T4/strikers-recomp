"""Create lossless labeled contact sheets of native PrintWindow PNG evidence."""
from pathlib import Path
import json
import argparse
from PIL import Image,ImageDraw
ROOT=Path(__file__).resolve().parents[2]

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--contains',action='append',default=[])
    parser.add_argument('--name',default='job21-gui-review');args=parser.parse_args()
    out=ROOT/'build/phase8'/args.name
    if out.resolve().parent!=(ROOT/'build/phase8').resolve():raise ValueError('Review output escaped its folder')
    out.mkdir(exist_ok=True)
    pictures=sorted(p for p in (ROOT/'build/phase8').rglob('screenshots/*.png') if 'job21' in str(p).lower())
    if args.contains:pictures=[p for p in pictures if any(part in str(p) for part in args.contains)]
    index=[]
    for first in range(0,len(pictures),6):
        group=pictures[first:first+6]
        width=max(Image.open(p).width for p in group);height=max(Image.open(p).height for p in group)
        sheet=Image.new('RGB',(width*2,(height+42)*3),'white');draw=ImageDraw.Draw(sheet)
        for i,p in enumerate(group):
            x=(i%2)*width;y=(i//2)*(height+42)
            draw.text((x+4,y+3),p.parent.parent.name+' / '+p.name,fill='black')
            with Image.open(p) as im:sheet.paste(im,(x,y+40))
            index.append(dict(source=p.relative_to(ROOT).as_posix(),sheet=f'sheet-{first//6+1:02}.png'))
        sheet.save(out/f'sheet-{first//6+1:02}.png')
    (out/'index.json').write_text(json.dumps(index,indent=2))
    print(f'{len(pictures)} original PNGs; {(len(pictures)+5)//6} full-size sheets: {out}')

if __name__=='__main__':main()
